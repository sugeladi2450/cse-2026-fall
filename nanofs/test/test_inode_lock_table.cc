// Unit tests for InodeLockTable — the per-inode read/write lock mechanism.
// Concurrency is exercised with real std::threads; assertions check the three
// properties the lock table must give: writer mutual exclusion, reader overlap,
// and per-inode granularity (disjoint ids never serialize).
#include "nanofs/inode_lock_table.h"

#include <atomic>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace nanofs;
using namespace std::chrono_literals;

// Bump `peak` up to the largest value `cur` ever reaches (monotone max).
static void record_peak(std::atomic<int>& peak, int cur) {
  int prev = peak.load();
  while (cur > prev && !peak.compare_exchange_weak(prev, cur)) {}
}

// A single exclusive lock must serialize writers: at no instant may two threads
// be inside the critical section for the same inode.
TEST(InodeLockTable, ExclusiveIsMutuallyExclusive) {
  InodeLockTable locks(16);
  constexpr int kThreads = 8, kIters = 2000;
  std::atomic<int> inside{0};
  std::atomic<bool> violated{false};
  std::latch start{kThreads};

  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&] {
      start.arrive_and_wait();
      for (int i = 0; i < kIters; ++i) {
        auto g = locks.lock_exclusive(5);
        if (inside.fetch_add(1) != 0) violated = true;  // someone else already in
        inside.fetch_sub(1);
      }
    });
  }
  for (auto& th : ts) th.join();
  EXPECT_FALSE(violated) << "two writers were inside the same inode's lock";
}

// Multiple readers of the same inode must be able to hold the lock at once.
TEST(InodeLockTable, SharedLocksOverlap) {
  InodeLockTable locks(16);
  constexpr int kReaders = 6;
  std::atomic<int> concurrent{0}, peak{0};
  std::latch start{kReaders};

  std::vector<std::thread> ts;
  for (int t = 0; t < kReaders; ++t) {
    ts.emplace_back([&] {
      start.arrive_and_wait();
      auto g = locks.lock_shared(5);
      record_peak(peak, concurrent.fetch_add(1) + 1);
      std::this_thread::sleep_for(20ms);  // hold long enough to force overlap
      concurrent.fetch_sub(1);
    });
  }
  for (auto& th : ts) th.join();
  EXPECT_GE(peak.load(), 2) << "readers did not overlap — shared lock behaves exclusive";
}

// Exclusive locks on *different* inodes must not serialize: the lock is
// per-inode, not one global lock.
TEST(InodeLockTable, DisjointExclusiveRunConcurrently) {
  InodeLockTable locks(16);
  std::atomic<int> concurrent{0}, peak{0};
  std::latch start{2};

  auto worker = [&](inode_id id) {
    start.arrive_and_wait();
    auto g = locks.lock_exclusive(id);
    record_peak(peak, concurrent.fetch_add(1) + 1);
    std::this_thread::sleep_for(20ms);
    concurrent.fetch_sub(1);
  };
  std::thread a(worker, 1), b(worker, 2);
  a.join();
  b.join();
  EXPECT_EQ(peak.load(), 2) << "disjoint inodes serialized — lock is not per-inode";
}
