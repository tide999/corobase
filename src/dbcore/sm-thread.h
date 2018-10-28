#pragma once

#include <numa.h>

#include <condition_variable>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

#include <sys/stat.h>

#include "sm-defs.h"
#include "xid.h"
#include "../util.h"

namespace ermia {
namespace thread {

struct CPUCore {
  uint32_t node;
  uint32_t physical_thread;
  std::vector<uint32_t> logical_threads;
  CPUCore(uint32_t n, uint32_t phys) : node(n), physical_thread(phys) {}
  void AddLogical(uint32_t t) { logical_threads.push_back(t); }
};

extern std::vector<CPUCore> cpu_cores;

bool DetectCPUCores();
void Initialize();

extern uint32_t
    next_thread_id;  // == total number of threads had so far - never decreases
extern __thread uint32_t thread_id;
extern __thread bool thread_initialized;

inline uint32_t MyId() {
  if (!thread_initialized) {
    thread_id = __sync_fetch_and_add(&next_thread_id, 1);
    thread_initialized = true;
  }
  return thread_id;
}

struct Thread {
  const uint8_t kStateHasWork = 1U;
  const uint8_t kStateSleep = 2U;
  const uint8_t kStateNoWork = 3U;

  typedef std::function<void(char *task_input)> Task;
  std::thread thd;
  uint16_t node;
  uint16_t core;
  uint32_t sys_cpu;  // OS-given CPU number
  bool shutdown;
  uint8_t state;
  Task task;
  char *task_input;
  bool sleep_when_idle;
  bool is_physical;

  std::condition_variable trigger;
  std::mutex trigger_lock;

  Thread(uint16_t n, uint16_t c, uint32_t sys_cpu, bool is_physical);
  ~Thread() {}

  void IdleTask();

  // No CC whatsoever, caller must know what it's doing
  inline void StartTask(Task t, char *input = nullptr) {
    task = t;
    task_input = input;
    auto s = __sync_val_compare_and_swap(&state, kStateNoWork, kStateHasWork);
    if (s == kStateSleep) {
      while (volatile_read(state) != kStateNoWork) {
        trigger.notify_all();
      }
      volatile_write(state, kStateHasWork);
    } else {
      ALWAYS_ASSERT(s == kStateNoWork);
    }
  }

  inline void Join() { while (volatile_read(state) == kStateHasWork) {} }
  inline bool TryJoin() { return volatile_read(state) != kStateHasWork; }
  inline void Destroy() { volatile_write(shutdown, true); }
};

struct PerNodeThreadPool {
  static uint32_t max_threads_per_node;
  uint16_t node CACHE_ALIGNED;
  Thread *threads CACHE_ALIGNED;
  uint64_t bitmap CACHE_ALIGNED;  // max 64 threads per node, 1 - busy, 0 - free

  // Get a single new thread which can be physical or logical
  inline Thread *GetThread(bool physical) {
  retry:
    uint64_t b = volatile_read(bitmap);
    uint64_t xor_pos = b ^ (~uint64_t{0});
    uint64_t pos = __builtin_ctzll(xor_pos);
    if (pos == max_threads_per_node) {
      return nullptr;
    }

    Thread *t = threads + pos;
    // Find the thread that matches the preferred type 
    while (true) {
      ++pos;
      if (pos >= max_threads_per_node) {
        return nullptr;
      }
      t = &threads[pos];
      if ((!((1UL << pos) & b)) && (t->is_physical == physical)) {
        break;
      }
    }

    if (not __sync_bool_compare_and_swap(&bitmap, b, b | (1UL << pos))) {
      goto retry;
    }
    ALWAYS_ASSERT(pos < max_threads_per_node);
    return t;
  }

  // Get a thread group - which includes all the threads (phyiscal + logical) on
  // the same phyiscal core. Similar to GetThread, but continue to also allocate
  // the logical threads that follow immediately the physical thread in the
  // bitmap.
  inline bool GetThreadGroup(std::vector<Thread*> &thread_group) {
  retry:
    thread_group.clear();
    uint64_t b = volatile_read(bitmap);
    uint64_t xor_pos = b ^ (~uint64_t{0});
    uint64_t pos = __builtin_ctzll(xor_pos);
    if (pos == max_threads_per_node) {
      return false;
    }

    Thread *t = threads + pos;
    // Find the thread that matches the preferred type 
    while (true) {
      ++pos;
      if (pos >= max_threads_per_node) {
        return false;
      }
      t = &threads[pos];
      if ((!((1UL << pos) & b)) && t->is_physical) {
        break;
      }
    }

    thread_group.push_back(t);

    // Got the physical thread, now try to claim the logical ones as well
    uint64_t count = 1;  // Number of 1-bits, including the physical thread
    ++pos;
    while (true) {
      t = threads + pos;
      if (t->is_physical) {
        break;
      } else {
        thread_group.push_back(t);
        ++count;
      }
    }

    // Fill [count] bits starting from [pos]
    uint64_t bits = 0;
    for (uint32_t i = pos; i < pos + count; ++i) {
      bits |= (1UL << pos);
    }
    if (not __sync_bool_compare_and_swap(&bitmap, b, b | bits)) {
      goto retry;
    }
    ALWAYS_ASSERT(pos < max_threads_per_node);
    return true;
  }

  inline void PutThread(Thread *t) {
    auto b = ~uint64_t{1UL << (t - threads)};
    __sync_fetch_and_and(&bitmap, b);
  }

  PerNodeThreadPool(uint16_t n);
};

extern PerNodeThreadPool *thread_pools;

inline Thread *GetThread(uint16_t from, bool physical) {
  return thread_pools[from].GetThread(physical);
}

inline Thread *GetThread(bool physical /* don't care where */) {
  for (uint16_t i = 0; i < config::numa_nodes; i++) {
    auto *t = thread_pools[i].GetThread(physical);
    if (t) {
      return t;
    }
  }
  return nullptr;
}

// Return all the threads (physical + logical) on the same physical core
inline bool GetThreadGroup(uint16_t from, std::vector<Thread*> &threads) {
  return thread_pools[from].GetThreadGroup(threads);
}

inline bool GetThreadGroup(std::vector<Thread*> &threads /* don't care where */) {
  for (uint16_t i = 0; i < config::numa_nodes; i++) {
    if (thread_pools[i].GetThreadGroup(threads)) {
      break;
    }
  }
  return threads.size() > 0;
}

inline void PutThread(Thread *t) { thread_pools[t->node].PutThread(t); }

// A wrapper that includes Thread for user code to use.
// Benchmark and log replay threads deal with this only,
// not with Thread.
struct Runner {
  Runner(bool physical = true) : me(nullptr), physical(physical) {}
  ~Runner() {
    if (me) {
      Join();
    }
  }

  virtual void MyWork(char *) = 0;

  inline void Start() {
    ALWAYS_ASSERT(me);
    thread::Thread::Task t =
        std::bind(&Runner::MyWork, this, std::placeholders::_1);
    me->StartTask(t);
  }

  inline bool TryImpersonate(bool sleep_when_idle = true) {
    ALWAYS_ASSERT(not me);
    me = thread::GetThread(physical);
    if (me) {
      LOG_IF(FATAL, me->is_physical != physical) << "Not the requested thread type";
      me->sleep_when_idle = sleep_when_idle;
    }
    return me != nullptr;
  }

  inline void Join() {
    me->Join();
    PutThread(me);
    me = nullptr;
  }
  // Same as Join(), but don't return the thread
  inline void Wait() { me->Join(); }
  inline bool TryWait() { return me->TryJoin(); }
  inline bool IsImpersonated() { return me != nullptr; }
  inline bool TryJoin() {
    if (me->TryJoin()) {
      PutThread(me);
      me = nullptr;
      return true;
    }
    return false;
  }

  Thread *me;
  bool physical;
};
}  // namespace thread
}  // namespace ermia
