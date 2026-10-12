// Concurrent-correctness tests for LockedFs — before-or-after atomicity under
// real thread contention. LockedFs stacks per-inode read/write locks + the
// allocator lock on top of TxFs/JournaledBlockDevice. std::threads stand in for the
// concurrent clients (FUSE worker threads / subagents).
#include <nanofs/locked_fs.h>
#include <nanofs/block.h>
#include <nanofs/inode.h>
#include <nanofs/journal.h>

#include <atomic>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace nanofs;

namespace {
// Build a formatted, checkpointed fs stack behind a LockedFs. Members are
// declared in dependency order so destruction unwinds top-down.
struct Fs {
  static constexpr uint32_t kInodes = 64;
  SimulatedBlockDevice image{4096, 256}, jbk{4096, 256};
  JournaledBlockDevice j{image, jbk, /*threshold=*/192};
  InodeManager im{j, kInodes};
  LockedFs fs{im, j, kInodes};
  Fs() {
    EXPECT_TRUE(im.format().has_value());
    EXPECT_TRUE(j.checkpoint().has_value());
  }

  Superblock super() {
    std::vector<std::byte> buf(j.block_size());
    EXPECT_TRUE(j.read_block(0, buf).has_value());
    Superblock sb{};
    std::memcpy(&sb, buf.data(), sizeof(sb));
    return sb;
  }

  // Bits set in the data-block bitmap. A lost update on a shared bitmap block
  // shows up here as a count that drifts from the expected one — up if freed
  // bits were resurrected (leak), down if allocated bits were cleared.
  uint32_t allocated_blocks() {
    Superblock sb = super();
    BlockAllocator ba(j, sb.block_bitmap_start, sb.block_bitmap_blocks, sb.data_start,
                      sb.num_blocks);
    uint32_t n = 0;
    for (block_id b = sb.data_start; b < sb.num_blocks; ++b)
      if (ba.is_allocated(b).value_or(false)) ++n;
    return n;
  }

  uint32_t allocated_inodes() {
    Superblock sb = super();
    BlockAllocator ia(j, sb.inode_bitmap_start, sb.inode_bitmap_blocks, ROOT_INODE,
                      sb.num_inodes + 1);
    uint32_t n = 0;
    for (inode_id id = ROOT_INODE; id <= sb.num_inodes; ++id)
      if (ia.is_allocated(id).value_or(false)) ++n;
    return n;
  }

  // The direct block pointers a file currently owns.
  std::vector<block_id> direct_blocks(inode_id id) {
    auto node = im.read_inode(id);
    EXPECT_TRUE(node.has_value());
    std::vector<block_id> out;
    for (uint32_t i = 0; i < NDIRECT; ++i)
      if (node->blocks[i] != NULL_BLOCK) out.push_back(node->blocks[i]);
    return out;
  }
};
}  // namespace

// Concurrent creates in *different* directories exercise the allocator lock:
// disjoint parent locks let the creates run concurrently, and the allocator lock
// must still hand out distinct inodes/blocks. Every file must exist afterward and
// no inode id may be handed out twice.
TEST(LockedFsConcurrent, ConcurrentCreateDifferentDirsNoDoubleAlloc) {
  Fs t;
  constexpr int kN = 10;
  std::vector<inode_id> dirs(kN);
  for (int i = 0; i < kN; ++i) {
    auto d = t.fs.mkdir(ROOT_INODE, "d" + std::to_string(i));
    ASSERT_TRUE(d.has_value());
    dirs[i] = *d;
  }

  std::vector<inode_id> made(kN, 0);
  std::vector<std::thread> ts;
  for (int i = 0; i < kN; ++i) {
    ts.emplace_back([&, i] {
      auto f = t.fs.create(dirs[i], "file");
      if (f) made[i] = *f;
    });
  }
  for (auto& th : ts) th.join();

  std::set<inode_id> uniq;
  for (int i = 0; i < kN; ++i) {
    ASSERT_NE(made[i], 0u) << "create in d" << i << " failed";
    EXPECT_TRUE(uniq.insert(made[i]).second) << "inode id " << made[i] << " handed out twice";
    auto look = t.fs.lookup(dirs[i], "file");
    ASSERT_TRUE(look.has_value()) << "file in d" << i << " missing";
    EXPECT_EQ(*look, made[i]);
  }
}

// Concurrent creates in the *same* directory serialize on the parent's
// exclusive lock; none may be lost or leave a torn directory.
TEST(LockedFsConcurrent, ConcurrentCreateSameDirAllPresent) {
  Fs t;
  constexpr int kN = 10;
  std::vector<std::thread> ts;
  for (int i = 0; i < kN; ++i)
    ts.emplace_back([&, i] { (void)t.fs.create(ROOT_INODE, "f" + std::to_string(i)); });
  for (auto& th : ts) th.join();

  auto entries = t.fs.readdir(ROOT_INODE);
  ASSERT_TRUE(entries.has_value());
  std::set<std::string> names;
  for (const auto& e : *entries) names.insert(e.name);
  for (int i = 0; i < kN; ++i)
    EXPECT_TRUE(names.count("f" + std::to_string(i))) << "f" << i << " lost";
}

// A reader concurrent with a writer must observe a whole prior or whole new
// value, never a torn mix — the per-inode lock gives before-or-after. The value
// spans two blocks: the device already makes one block atomic, so a reader that
// skips the lock could only be caught between the writer's two block writes.
// The reader runs for the writer's whole lifetime and every op must succeed.
TEST(LockedFsConcurrent, ReadWriteIsolationNoTear) {
  Fs t;
  auto f = t.fs.create(ROOT_INODE, "x");
  ASSERT_TRUE(f.has_value());
  const inode_id id = *f;
  const uint32_t n = 2 * t.j.block_size();
  const std::string A(n, 'A'), B(n, 'B');
  ASSERT_TRUE(t.fs.write_file(id, 0, A).has_value());

  std::atomic<bool> done{false};
  std::atomic<int> torn{0}, failed{0}, reads{0};
  std::thread writer([&] {
    for (int i = 0; i < 200; ++i)
      if (!t.fs.write_file(id, 0, (i & 1) ? B : A)) ++failed;
    done = true;
  });
  std::thread reader([&] {
    while (!done) {
      auto r = t.fs.read_file(id, 0, n);
      ++reads;
      if (!r) ++failed;
      else if (*r != A && *r != B) ++torn;
    }
  });
  writer.join();
  reader.join();
  EXPECT_EQ(failed, 0) << "an op returned an error";
  EXPECT_EQ(torn, 0) << "reader observed a torn (half-updated) value in " << reads << " reads";
}

// Allocation and *release* running at the same time — the case per-inode locks
// cannot see. Growing a file (write_file) sets bits in the block bitmap while
// setattr(0) on another file clears bits in the same bitmap block; each op
// caches its own copy of that block in its write set, so without the allocator
// lock held from the first allocation through commit the later committer
// overwrites the earlier's bits. The files are disjoint inodes, so nothing else
// serializes them. Every thread alternates grow/shrink, ending grown, which
// makes the final bitmap population exact: any lost update shows up as a block
// count that drifted, a block id handed to two files, or a live block read as free.
TEST(LockedFsConcurrent, ConcurrentGrowShrinkNoBlockLeakOrDoubleAlloc) {
  Fs t;
  constexpr int kThreads = 8, kRounds = 20, kBlocksPerFile = 4;
  const uint32_t bs = t.j.block_size();

  std::vector<inode_id> files(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    auto f = t.fs.create(ROOT_INODE, "f" + std::to_string(i));
    ASSERT_TRUE(f.has_value());
    files[i] = *f;
  }
  const uint32_t baseline = t.allocated_blocks();   // all files empty here

  std::vector<std::thread> ts;
  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back([&, i] {
      for (int r = 0; r <= kRounds; ++r) {
        for (int b = 0; b < kBlocksPerFile; ++b)     // grow: allocate one block per write
          (void)t.fs.write_file(files[i], b * bs, "x");
        if (r < kRounds) (void)t.fs.setattr(files[i], 0);   // shrink: free them again
      }                                              // last round leaves the file grown
    });
  }
  for (auto& th : ts) th.join();

  Superblock sb = t.super();
  BlockAllocator ba(t.j, sb.block_bitmap_start, sb.block_bitmap_blocks, sb.data_start,
                    sb.num_blocks);
  std::set<block_id> live;
  for (int i = 0; i < kThreads; ++i) {
    auto blocks = t.direct_blocks(files[i]);
    EXPECT_EQ(blocks.size(), kBlocksPerFile) << "f" << i << " lost blocks";
    for (block_id b : blocks) {
      EXPECT_TRUE(live.insert(b).second) << "block " << b << " handed out twice";
      EXPECT_TRUE(ba.is_allocated(b).value_or(false))
          << "live block " << b << " of f" << i << " reads as free";
    }
  }
  EXPECT_EQ(t.allocated_blocks(), baseline + kThreads * kBlocksPerFile)
      << "block bitmap population drifted: a concurrent free/alloc clobbered it";
}

// The inode-bitmap twin of the test above: mkdir allocates an inode (and its
// structure block) while a concurrent rmdir frees one. Each thread works under
// its *own* parent, so the parent locks are disjoint and the two ops really do
// overlap; only the allocator lock can keep the shared bitmap blocks consistent.
TEST(LockedFsConcurrent, ConcurrentMkdirRmdirNoInodeLeakOrDoubleAlloc) {
  Fs t;
  constexpr int kThreads = 8, kRounds = 20;

  std::vector<inode_id> parents(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    auto d = t.fs.mkdir(ROOT_INODE, "p" + std::to_string(i));
    ASSERT_TRUE(d.has_value());
    parents[i] = *d;
    // Warm-up round: let every parent grow its own directory data block (allocated on
    // the first child, never reclaimed by rmdir) so the baseline is the steady state
    // of the concurrent phase; otherwise the count is off by kThreads blocks for
    // reasons unrelated to locking.
    ASSERT_TRUE(t.fs.mkdir(parents[i], "child").has_value());
    ASSERT_TRUE(t.fs.rmdir(parents[i], "child").has_value());
  }
  const uint32_t baseline_inodes = t.allocated_inodes();
  const uint32_t baseline_blocks = t.allocated_blocks();

  std::vector<std::thread> ts;
  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back([&, i] {
      for (int r = 0; r <= kRounds; ++r) {
        (void)t.fs.mkdir(parents[i], "child");
        if (r < kRounds) (void)t.fs.rmdir(parents[i], "child");   // last one survives
      }
    });
  }
  for (auto& th : ts) th.join();

  Superblock sb = t.super();
  BlockAllocator ia(t.j, sb.inode_bitmap_start, sb.inode_bitmap_blocks, ROOT_INODE,
                    sb.num_inodes + 1);
  std::set<inode_id> live;
  for (int i = 0; i < kThreads; ++i) {
    auto child = t.fs.lookup(parents[i], "child");
    ASSERT_TRUE(child.has_value()) << "p" << i << "/child lost";
    EXPECT_TRUE(live.insert(*child).second) << "inode " << *child << " handed out twice";
    EXPECT_TRUE(ia.is_allocated(*child).value_or(false))
        << "live inode " << *child << " reads as free";
  }
  EXPECT_EQ(t.allocated_inodes(), baseline_inodes + kThreads)
      << "inode bitmap population drifted: a concurrent free/alloc clobbered it";
  EXPECT_EQ(t.allocated_blocks(), baseline_blocks + kThreads)
      << "one structure block per surviving dir — block bitmap population drifted";
}

// Mixed create/unlink across dirs must never deadlock: all threads finish.
TEST(LockedFsConcurrent, ConcurrentCreateUnlinkNoDeadlock) {
  Fs t;
  constexpr int kThreads = 8, kIters = 100;
  std::atomic<int> failed{0};
  std::vector<std::thread> ts;
  for (int th = 0; th < kThreads; ++th) {
    ts.emplace_back([&, th] {
      std::string name = "t" + std::to_string(th);
      for (int i = 0; i < kIters; ++i) {
        if (!t.fs.create(ROOT_INODE, name)) ++failed;
        if (!t.fs.unlink(ROOT_INODE, name)) ++failed;
      }
    });
  }
  for (auto& th : ts) th.join();
  EXPECT_EQ(failed, 0) << "create/unlink must also succeed";
  auto entries = t.fs.readdir(ROOT_INODE);
  ASSERT_TRUE(entries.has_value());
  EXPECT_TRUE(entries->empty());
}
