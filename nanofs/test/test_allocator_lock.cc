// Unit tests for AllocatorLock — the lazy lock that serializes allocation.
//
// Model: each fs op opens an AllocatorLock::Scope (RAII). The allocator calls
// AllocatorLock::engage_current() the first time it allocates/frees; that
// engages the lock's mutex for this thread's active scope and holds it until
// the scope is destroyed (after commit). Ops that never allocate never engage,
// so they never touch the mutex and never block anyone.
#include "nanofs/allocator_lock.h"

#include <atomic>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace nanofs;
using namespace std::chrono_literals;

static void record_peak(std::atomic<int>& peak, int cur) {
  int prev = peak.load();
  while (cur > prev && !peak.compare_exchange_weak(prev, cur)) {}
}

// Two ops that both engage the lock must serialize: never both inside at once.
TEST(AllocatorLock, EngagedScopesSerialize) {
  AllocatorLock alock;
  std::atomic<int> concurrent{0}, peak{0};
  std::latch start{2};

  auto worker = [&] {
    start.arrive_and_wait();
    AllocatorLock::Scope scope(alock);
    AllocatorLock::engage_current();  // allocator would call this
    record_peak(peak, concurrent.fetch_add(1) + 1);
    std::this_thread::sleep_for(20ms);
    concurrent.fetch_sub(1);
  };
  std::thread a(worker), b(worker);
  a.join();
  b.join();
  EXPECT_EQ(peak.load(), 1) << "two engaged scopes ran concurrently — not serialized";
}

// An op that never engages must not block one that does: no shared mutex is
// taken by the un-engaged scope, so the two overlap.
TEST(AllocatorLock, UnengagedScopeDoesNotBlockEngaged) {
  AllocatorLock alock;
  std::atomic<int> concurrent{0}, peak{0};
  std::latch start{2};

  std::thread engaged([&] {
    start.arrive_and_wait();
    AllocatorLock::Scope scope(alock);
    AllocatorLock::engage_current();
    record_peak(peak, concurrent.fetch_add(1) + 1);
    std::this_thread::sleep_for(20ms);
    concurrent.fetch_sub(1);
  });
  std::thread bystander([&] {
    start.arrive_and_wait();
    AllocatorLock::Scope scope(alock);  // opened but never engaged
    record_peak(peak, concurrent.fetch_add(1) + 1);
    std::this_thread::sleep_for(20ms);
    concurrent.fetch_sub(1);
  });
  engaged.join();
  bystander.join();
  EXPECT_EQ(peak.load(), 2) << "un-engaged scope blocked an engaged one";
}

// engage_current() is idempotent within one scope: a second call must be a
// no-op, not a re-lock (which would self-deadlock on a non-recursive mutex).
TEST(AllocatorLock, EngageIsIdempotentWithinScope) {
  AllocatorLock alock;
  bool reached_end = false;
  std::thread t([&] {
    AllocatorLock::Scope scope(alock);
    AllocatorLock::engage_current();
    AllocatorLock::engage_current();  // must NOT deadlock
    reached_end = true;
  });
  t.join();
  EXPECT_TRUE(reached_end) << "second engage_current() deadlocked";
}

// engage_current() with no active scope on this thread is a harmless no-op
// (e.g. an allocator call outside any LockedFs op).
TEST(AllocatorLock, EngageWithoutScopeIsNoop) {
  AllocatorLock::engage_current();  // no scope on this thread
  SUCCEED();
}
