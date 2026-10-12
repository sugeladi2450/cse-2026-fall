// Concurrency tests for JournaledBlockDevice's thread-keyed transactions.
//
// Two properties once the journal supports concurrent writers:
//   1. Isolation — a transaction's uncommitted write-set is visible only to the
//      thread that owns it; another thread reads `image`, never the buffered write.
//   2. Concurrent commits — two threads may each run a full transaction and
//      commit; both land durably and both survive crash + recover.
#include <nanofs/journal.h>
#include <nanofs/block.h>

#include <latch>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace nanofs;

namespace {
std::vector<std::byte> blk(size_t n, unsigned char v) {
  return std::vector<std::byte>(n, std::byte{v});
}
}  // namespace

// A reader on another thread must not observe an in-flight transaction's
// uncommitted metadata write. It sees the value in `image` instead.
TEST(JournalConcurrency, ReaderDoesNotSeeAnotherThreadsUncommittedWrite) {
  SimulatedBlockDevice image(64, 32), journal(64, 64);
  JournaledBlockDevice j(image, journal, /*threshold=*/48);
  const block_id id = 5;
  auto old = blk(64, 0xAA);
  ASSERT_TRUE(image.write_block(id, old).has_value());
  ASSERT_TRUE(image.flush().has_value());

  std::latch a_wrote{1}, b_read{1};
  std::vector<std::byte> seen(64);

  std::thread A([&] {
    ASSERT_TRUE(j.begin().has_value());
    ASSERT_TRUE(j.write_block(id, blk(64, 0xBB)).has_value());  // uncommitted
    a_wrote.count_down();
    b_read.wait();                                              // hold tx open, idle
    ASSERT_TRUE(j.commit().has_value());
  });
  std::thread B([&] {
    a_wrote.wait();
    ASSERT_TRUE(j.read_block(id, seen).has_value());            // B is in no tx
    b_read.count_down();
  });
  A.join();
  B.join();
  EXPECT_EQ(seen, old) << "reader saw another thread's uncommitted write";
}

// Two threads each commit a disjoint metadata block concurrently; both must be
// applied to image and both must be replayed after a crash.
TEST(JournalConcurrency, ConcurrentCommitsBothPersistAndRecover) {
  SimulatedBlockDevice image(64, 32), journal(64, 64);
  JournaledBlockDevice j(image, journal, /*threshold=*/48);
  std::latch start{2};

  auto committer = [&](block_id id, unsigned char v) {
    start.arrive_and_wait();
    ASSERT_TRUE(j.begin().has_value());
    ASSERT_TRUE(j.write_block(id, blk(64, v)).has_value());
    ASSERT_TRUE(j.commit().has_value());
  };
  std::thread a(committer, 10, 0xB1), b(committer, 11, 0xC2);
  a.join();
  b.join();

  std::vector<std::byte> v10(64), v11(64);
  ASSERT_TRUE(j.read_block(10, v10).has_value());
  ASSERT_TRUE(j.read_block(11, v11).has_value());
  EXPECT_EQ(v10, blk(64, 0xB1));
  EXPECT_EQ(v11, blk(64, 0xC2));

  j.reboot();                       // drop unflushed image writes
  ASSERT_TRUE(j.recover().has_value());     // both committed txs replay
  ASSERT_TRUE(j.read_block(10, v10).has_value());
  ASSERT_TRUE(j.read_block(11, v11).has_value());
  EXPECT_EQ(v10, blk(64, 0xB1)) << "committed tx lost after recover";
  EXPECT_EQ(v11, blk(64, 0xC2)) << "committed tx lost after recover";
}
