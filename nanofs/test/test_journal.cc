#include <nanofs/journal.h>
#include <nanofs/block.h>
#include <nanofs/fs.h>
#include <nanofs/txfs.h>
#include <nanofs/inode.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>
using namespace nanofs;

namespace {
std::vector<std::byte> blk(size_t n, unsigned char v) { return std::vector<std::byte>(n, std::byte{v}); }
}

// A metadata write inside a transaction goes only to the write set; the image does not see it before commit.
TEST(Journal, UncommittedWritesInvisibleToImage) {
  SimulatedBlockDevice image(64, 32), jdev_backing(64, 64);
  JournaledBlockDevice j(image, jdev_backing, /*threshold=*/48);
  auto w = blk(64, 0x11);
  ASSERT_TRUE(j.begin().has_value());
  ASSERT_TRUE(j.write_block(10, w).has_value());
  std::vector<std::byte> via_j(64), via_image(64);
  ASSERT_TRUE(j.read_block(10, via_j).has_value());
  EXPECT_EQ(via_j, w);                                  // the transaction reads its own write
  ASSERT_TRUE(image.read_block(10, via_image).has_value());
  EXPECT_EQ(via_image, blk(64, 0x00));                   // image untouched
  ASSERT_TRUE(j.abort().has_value());
  ASSERT_TRUE(image.read_block(10, via_image).has_value());
  EXPECT_EQ(via_image, blk(64, 0x00));                   // still untouched after abort
}

// Data bypass: write_block(..., BlockKind::Data) lands on the image directly (never enters the write set).
TEST(Journal, DataBlockBypassesJournal) {
  SimulatedBlockDevice image(64, 32), jdev_backing(64, 64);
  JournaledBlockDevice j(image, jdev_backing, 48);
  auto d = blk(64, 0x22);
  ASSERT_TRUE(j.begin().has_value());
  ASSERT_TRUE(j.write_block(7, d, BlockKind::Data).has_value());
  std::vector<std::byte> via_image(64);
  ASSERT_TRUE(image.read_block(7, via_image).has_value());
  EXPECT_EQ(via_image, d);                               // data reaches the image at once (volatile)
}

// The commit half only: after commit the image sees the write set at once (applied).
// The full commit + crash + recover scenario is CommitAppliesToImageAndIsRecoverable
// below; this one stays green before recover() is written.
TEST(Journal, CommitAppliesToImage) {
  SimulatedBlockDevice image(64, 32), jbk(64, 64);
  JournaledBlockDevice j(image, jbk, 48);
  auto a = std::vector<std::byte>(64, std::byte{0xA1});
  ASSERT_TRUE(j.begin().has_value());
  ASSERT_TRUE(j.write_block(4, a).has_value());
  ASSERT_TRUE(j.commit().has_value());
  std::vector<std::byte> r(64);
  ASSERT_TRUE(image.read_block(4, r).has_value());
  EXPECT_EQ(r, a);                                // commit applied to the image (volatile)
}

// One descriptor block's tail holds at most (bs-sizeof(JournalDesc))/4 block ids; a
// larger write set must be rejected before touching the log. Error == the transaction
// never happened: image unchanged, no leftover in the log, this thread can begin again.
TEST(Journal, CommitRejectsOversizedTransaction) {
  SimulatedBlockDevice image(64, 32), jbk(64, 64);   // bs=64 => at most (64-12)/4 = 13 records
  JournaledBlockDevice j(image, jbk, /*threshold=*/999);
  ASSERT_TRUE(j.begin().has_value());
  for (block_id id = 0; id < 14; ++id)
    ASSERT_TRUE(j.write_block(id, blk(64, 0x33)).has_value());
  auto r = j.commit();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), Error::NoSpace);
  std::vector<std::byte> v(64);
  ASSERT_TRUE(image.read_block(0, v).has_value());
  EXPECT_EQ(v, blk(64, 0x00));                       // image untouched
  JournalDesc d; std::vector<std::byte> desc(64);
  ASSERT_TRUE(jbk.read_block(0, desc).has_value());
  std::memcpy(&d, desc.data(), sizeof(d));
  EXPECT_NE(d.magic, NANOFS_JRNL_MAGIC);             // no leftover descriptor in the log
  ASSERT_TRUE(j.begin().has_value());                // the failed commit cleared this thread's transaction
  ASSERT_TRUE(j.abort().has_value());
}

// After commit: the image sees the write at once (applied), and after a crash loses the image's volatile layer it is recovered from the journal.
TEST(Journal, CommitAppliesToImageAndIsRecoverable) {
  SimulatedBlockDevice image(64, 32), jbk(64, 64);
  JournaledBlockDevice j(image, jbk, 48);
  auto a = std::vector<std::byte>(64, std::byte{0xA1});
  ASSERT_TRUE(j.begin().has_value());
  ASSERT_TRUE(j.write_block(4, a).has_value());
  ASSERT_TRUE(j.commit().has_value());
  std::vector<std::byte> r(64);
  ASSERT_TRUE(image.read_block(4, r).has_value());
  EXPECT_EQ(r, a);                                // commit applied to the image (volatile)
  // the journal is durable: the image can lose power, the journal backing keeps it
  image.reboot();                          // drop the image's unflushed writes (the apply included)
  std::vector<std::byte> r2(64);
  ASSERT_TRUE(image.read_block(4, r2).has_value());
  EXPECT_EQ(r2, std::vector<std::byte>(64, std::byte{0}));  // after the crash the image is back to 0
  ASSERT_TRUE(j.recover().has_value());           // redo from the journal
  ASSERT_TRUE(image.read_block(4, r2).has_value());
  EXPECT_EQ(r2, a);                               // after recover the image holds the committed value
}

// Torn tail: one complete committed transaction + one corrupted descriptor => only the complete one is redone.
TEST(Journal, RecoverRedoesCommittedStopsAtTornTail) {
  SimulatedBlockDevice image(64, 32), jbk(64, 64);
  {
    JournaledBlockDevice j(image, jbk, /*threshold=*/999);   // never checkpoints
    ASSERT_TRUE(j.begin().has_value());
    ASSERT_TRUE(j.write_block(6, std::vector<std::byte>(64, std::byte{0x66})).has_value());
    ASSERT_TRUE(j.commit().has_value());                    // transaction 0 (journal blocks 0,1)
  }
  // Hand-write a "half descriptor" into journal block 2 (right magic, records missing / bad checksum) to fake a torn tail.
  std::vector<std::byte> torn(64, std::byte{0});
  JournalDesc bad{NANOFS_JRNL_MAGIC, 1, 0xDEADBEEF};
  std::memcpy(torn.data(), &bad, sizeof(bad));
  ASSERT_TRUE(jbk.write_block(2, torn).has_value());
  ASSERT_TRUE(jbk.flush().has_value());

  image.reboot();                                    // drop the image's volatile layer
  JournaledBlockDevice j2(image, jbk, 999);
  ASSERT_TRUE(j2.recover().has_value());
  std::vector<std::byte> r(64);
  ASSERT_TRUE(image.read_block(6, r).has_value());
  EXPECT_EQ(r, std::vector<std::byte>(64, std::byte{0x66}));  // the complete transaction was redone
}

// After checkpoint: the image is durable (survives a crash without recover) and the journal is reset (recover has nothing to do).
TEST(Journal, CheckpointPersistsImageAndResetsJournal) {
  SimulatedBlockDevice image(64, 32), jbk(64, 64);
  JournaledBlockDevice j(image, jbk, /*threshold=*/1);   // checkpoint after every commit
  auto v = std::vector<std::byte>(64, std::byte{0x5A});
  ASSERT_TRUE(j.begin().has_value());
  ASSERT_TRUE(j.write_block(9, v).has_value());
  ASSERT_TRUE(j.commit().has_value());                  // triggers the checkpoint
  image.reboot();                                // drop the image's volatile layer
  std::vector<std::byte> r(64);
  ASSERT_TRUE(image.read_block(9, r).has_value());
  EXPECT_EQ(r, v);                                       // checkpoint flushed: the image is durable
  // journal reset: the block-0 descriptor is invalid => recover redoes nothing
  JournalDesc d; std::vector<std::byte> desc(64);
  ASSERT_TRUE(jbk.read_block(0, desc).has_value());
  std::memcpy(&d, desc.data(), sizeof(d));
  EXPECT_NE(d.magic, NANOFS_JRNL_MAGIC);
}

// Regression: a descriptor left over from before a checkpoint must not be replayed by recover.
TEST(Journal, RecoverIgnoresStaleDescriptorsAfterCheckpoint) {
  SimulatedBlockDevice image(64, 32), jbk(64, 64);
  JournaledBlockDevice j(image, jbk, /*threshold=*/4);
  // Before the checkpoint: two transactions with 1-block write sets; the second (tx B) puts its descriptor at journal block 2.
  ASSERT_TRUE(j.begin().has_value());
  ASSERT_TRUE(j.write_block(10, blk(64, 0xA1)).has_value());   // tx A -> image[10]
  ASSERT_TRUE(j.commit().has_value());                          // cursor 0->2
  ASSERT_TRUE(j.begin().has_value());
  ASSERT_TRUE(j.write_block(20, blk(64, 0xB1)).has_value());   // tx B -> image[20], desc@2
  ASSERT_TRUE(j.commit().has_value());                          // cursor 2->4 >=4 => checkpoint
  // After the checkpoint: one 1-block transaction over image[20], using only journal blocks 0,1 (block 2's old descB is not overwritten).
  ASSERT_TRUE(j.begin().has_value());
  ASSERT_TRUE(j.write_block(20, blk(64, 0xC1)).has_value());   // tx C -> image[20]=0xC1
  ASSERT_TRUE(j.commit().has_value());                          // cursor 0->2 (<4, no checkpoint)
  image.reboot();                                        // drop the image's volatile layer
  ASSERT_TRUE(j.recover().has_value());
  std::vector<std::byte> r(64);
  ASSERT_TRUE(image.read_block(20, r).has_value());
  EXPECT_EQ(r, blk(64, 0xC1));   // must be the post-checkpoint value; replaying the stale descB would roll it back to 0xB1
}

// create through the fs over the journal: crash after commit + recover => the file exists (the "all" of all-or-nothing).
TEST(JournalFs, CreateSurvivesCrashAfterCommit) {
  SimulatedBlockDevice image(4096, 256), jbk(4096, 256);
  JournaledBlockDevice j(image, jbk, /*threshold=*/192);
  InodeManager im(j, 64);
  ASSERT_TRUE(im.format().has_value());
  ASSERT_TRUE(j.checkpoint().has_value());       // persist format's writes + clear the journal
  {
    TxFs fs(im, j);                               // TxFs draws the transaction boundary (FsLayer alone does not journal)
    auto id = fs.create(ROOT_INODE, "a.txt");     // one transaction: begin...commit
    ASSERT_TRUE(id.has_value());
  }
  image.reboot();                          // drop the image's volatile layer
  ASSERT_TRUE(j.recover().has_value());
  FsLayer fs2(im, j);
  auto look = fs2.lookup(ROOT_INODE, "a.txt");
  ASSERT_TRUE(look.has_value());                  // found after recover => create took full effect
}

// mini-fsck: structural consistency that must hold after recover, whether or not the
// op took effect. It complements the listing comparison in expect_all_or_nothing: an
// inode with no directory entry is invisible to readdir, so only the bitmap walk below
// catches a leaked one. A torn op leaves a half-built structure: one of the three
// invariants below broken.
static void expect_consistent(InodeManager& im, FsLayer& fs, BlockDevice& dev,
                              const std::string& at_point) {
  std::vector<std::byte> sbuf(dev.block_size());
  ASSERT_TRUE(dev.read_block(0, sbuf).has_value());
  Superblock sb; std::memcpy(&sb, sbuf.data(), sizeof(sb));
  ASSERT_EQ(sb.magic, NANOFS_MAGIC) << "superblock destroyed at injection point " << at_point;

  BlockAllocator ialloc(dev, sb.inode_bitmap_start, sb.inode_bitmap_blocks,
                        ROOT_INODE, sb.num_inodes + 1);
  BlockAllocator balloc(dev, sb.block_bitmap_start, sb.block_bitmap_blocks,
                        sb.data_start, sb.num_blocks);

  auto entries = fs.readdir(ROOT_INODE);
  ASSERT_TRUE(entries.has_value()) << "root unreadable at injection point " << at_point;

  // 1. No dangling directory entry: every entry points at an allocated inode of a legal type.
  std::set<inode_id> referenced{ROOT_INODE};
  for (const auto& e : *entries) {
    auto used = ialloc.is_allocated(e.inode_id);
    ASSERT_TRUE(used.has_value());
    EXPECT_TRUE(*used) << "dangling dir entry '" << e.name << "' -> unallocated inode "
                       << e.inode_id << " at injection point " << at_point;
    if (!*used) continue;
    auto node = im.read_inode(e.inode_id);
    ASSERT_TRUE(node.has_value());
    EXPECT_NE(node->type, INODE_FREE) << "dir entry '" << e.name << "' -> FREE inode "
                                      << e.inode_id << " at injection point " << at_point;
    referenced.insert(e.inode_id);
  }

  // 2. No leaked inode: every allocated inode (except root) is referenced by some entry under root.
  for (inode_id id = ROOT_INODE + 1; id <= sb.num_inodes; ++id) {
    auto used = ialloc.is_allocated(id);
    ASSERT_TRUE(used.has_value());
    EXPECT_TRUE(!*used || referenced.count(id))
        << "leaked inode " << id << " (allocated but unreferenced) at injection point " << at_point;
  }

  // 3. No dangling data block: every direct block of a referenced inode is marked used in the block bitmap.
  for (inode_id id : referenced) {
    auto node = im.read_inode(id);
    ASSERT_TRUE(node.has_value());
    for (uint32_t i = 0; i < NDIRECT; ++i) {
      if (node->blocks[i] == NULL_BLOCK) continue;
      auto used = balloc.is_allocated(node->blocks[i]);
      ASSERT_TRUE(used.has_value());
      EXPECT_TRUE(*used) << "inode " << id << " points at free block " << node->blocks[i]
                         << " at injection point " << at_point;
    }
  }
}

// The stable state every crash point starts from: formatted, /seed.txt exists,
// everything checkpointed (journal empty, image flushed).
static void make_baseline(InodeManager& im, JournaledBlockDevice& j) {
  ASSERT_TRUE(im.format().has_value());
  ASSERT_TRUE(j.checkpoint().has_value());
  { TxFs fs(im, j); ASSERT_TRUE(fs.create(ROOT_INODE, "seed.txt").has_value()); }
  ASSERT_TRUE(j.checkpoint().has_value());
}

// The root directory as text, one "name ino type nlink size" line per entry, sorted.
// Two listings compare with ==, and gtest prints them on failure.
static std::string list_root(InodeManager& im, FsLayer& fs) {
  auto entries = fs.readdir(ROOT_INODE);
  EXPECT_TRUE(entries.has_value());
  if (!entries.has_value()) return "<root unreadable>\n";
  std::vector<std::string> lines;
  for (const auto& e : *entries) {
    std::string line = std::string(e.name) + " ino=" + std::to_string(e.inode_id);
    auto node = im.read_inode(e.inode_id);
    if (node.has_value())
      line += " type=" + std::to_string(node->type) + " nlink=" + std::to_string(node->nlink) +
              " size=" + std::to_string(node->size);
    lines.push_back(line + "\n");
  }
  std::sort(lines.begin(), lines.end());
  std::string out;
  for (const auto& l : lines) out += l;
  return out;
}

// For one op, lose power at every distinct point and check all-or-nothing after
// recover: the root listing must equal either the baseline's or the one a crash-free
// run of the op leaves behind (nothing happened, or exactly the op happened; block
// allocation is deterministic, so inode numbers match across runs).
// Dirty blocks reach the medium only in flush() (block.h), so the distinct
// crash points are exactly: "the nth flush of one device persisted k of its dirty
// blocks, then the machine died", for k = 0 .. all (crash_at_flush). The op is
// re-run from a fresh baseline for each (device, nth, k). Both inner loops end via
// crashed(): k past that flush's dirty count, or no nth flush on that device.
// A correct commit flushes only the journal inside a transaction, so the image
// round finds no flush at all; it is there to catch an implementation that persists
// image blocks before the commit point.
static void expect_all_or_nothing(const std::function<void(FsLayer&)>& op) {
  std::string before, done;                          // root listing at the baseline / after a crash-free op
  {
    SimulatedBlockDevice image(4096, 256), jbk(4096, 256);
    JournaledBlockDevice j(image, jbk, 192);
    InodeManager im(j, 64);
    make_baseline(im, j);
    { FsLayer fs(im, j); before = list_root(im, fs); }
    { TxFs fs(im, j); op(fs); }
    { FsLayer fs(im, j); done = list_root(im, fs); }
  }
  for (bool crash_image : {false, true}) {
    for (uint32_t nth = 1;; ++nth) {
      bool flush_exists = false;
      for (size_t k = 0;; ++k) {
        SimulatedBlockDevice image(4096, 256), jbk(4096, 256);
        JournaledBlockDevice j(image, jbk, 192);
        InodeManager im(j, 64);
        make_baseline(im, j);

        SimulatedBlockDevice& victim = crash_image ? image : jbk;
        victim.crash_at_flush(nth, k);
        { TxFs fs(im, j); op(fs); }                  // may lose power midway (TxFs provides the transaction; op dispatches virtually through FsLayer&)
        if (!victim.crashed()) break;                // k past this flush's dirty count (or no nth flush)
        flush_exists = true;

        image.reboot(); jbk.reboot();                // power loss is machine-wide: both devices lose their volatile layer
        const std::string point = std::string(crash_image ? "image" : "journal") + " flush " +
                                  std::to_string(nth) + ", " + std::to_string(k) + " blocks landed";
        ASSERT_TRUE(j.recover().has_value()) << "recover failed at " << point;
        FsLayer fs(im, j);
        expect_consistent(im, fs, j, point);         // no half-built structure, no leaked inode
        std::string now = list_root(im, fs);
        EXPECT_TRUE(now == before || now == done)    // nothing happened, or exactly the op happened
            << "op torn at " << point << "\nroot now:\n" << now
            << "baseline:\n" << before << "after crash-free op:\n" << done;
      }
      if (!flush_exists) break;                      // the op never flushed this device an nth time
    }
  }
}

TEST(JournalCrash, CreateAllOrNothing) {
  expect_all_or_nothing([](FsLayer& fs){ (void)fs.create(ROOT_INODE, "x.txt"); });
}
// Regression: file content bypasses the log, but allocating a data block first calls
// zero_block (into the write set). If commit overwrote the content already written to
// the image with that zero image, the data block would be wiped. Write one full block,
// read it back without a crash: it must be the original.
TEST(JournalFs, WriteFullBlockReadsBackNoCrash) {
  SimulatedBlockDevice image(4096, 256), jbk(4096, 256);
  JournaledBlockDevice j(image, jbk, 192);
  InodeManager im(j, 64);
  ASSERT_TRUE(im.format().has_value());
  ASSERT_TRUE(j.checkpoint().has_value());
  TxFs fs(im, j);   // only inside a transaction is there a write set for the zero-image regression to hit
  auto id = fs.create(ROOT_INODE, "a.txt");
  ASSERT_TRUE(id.has_value());
  std::string data(4096, 'A');
  ASSERT_TRUE(fs.write_file(*id, 0, data).has_value());
  auto r = fs.read_file(*id, 0, 4096);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, data);
}

// Regression: a multi-block, unaligned write at a high offset (the shape that exposed this bug in the stress test) must read back as written.
TEST(JournalFs, MultiBlockHighOffsetWriteReadsBack) {
  SimulatedBlockDevice image(4096, 256), jbk(4096, 256);
  JournaledBlockDevice j(image, jbk, 192);
  InodeManager im(j, 64);
  ASSERT_TRUE(im.format().has_value());
  ASSERT_TRUE(j.checkpoint().has_value());
  TxFs fs(im, j);
  auto id = fs.create(ROOT_INODE, "a.txt");
  ASSERT_TRUE(id.has_value());
  std::string data(15297, 'B');
  ASSERT_TRUE(fs.write_file(*id, 65373, data).has_value());
  auto r = fs.read_file(*id, 72129, 3994);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, std::string(3994, 'B'));
}

TEST(JournalCrash, UnlinkAllOrNothing) {
  // unlink's atomic unit is one transaction; the deleted file seed.txt already exists
  // in the stable baseline (the helper creates + checkpoints it), so op only unlinks.
  expect_all_or_nothing([](FsLayer& fs){ (void)fs.unlink(ROOT_INODE, "seed.txt"); });
}
