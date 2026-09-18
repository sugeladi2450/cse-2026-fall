// test_fs.cc · FsLayer unit tests (docs/lab1/lab1a.md)
//
// Suites are grouped by function. A private helper with no public entry is tested
// under the public function that calls it.
//
//   function                       │ test (suite.case)
//   ───────────────────────────────┼───────────────────────────────────────────
//   logical_to_physical_block      │ LogicalToPhysical.MapsTiersAndRejectsOverflow
//   read_bytes                     │ ReadBytes.TruncatesAndZeroFillsHoles
//   write_bytes                    │ WriteBytes.GrowsAndRewritesPartialBlock
//   read_file                      │ ReadFile.RejectsDirAndOverflow
//   write_file                     │ WriteFile.RejectsDirAndOverflow
//   getattr                        │ Getattr.ReportsTypeAndSize
//   setattr                        │ Setattr.ShrinkGrowIdempotent
//                                  │ Setattr.ShrinkGrowDoesNotAlias
//                                  │ Setattr.GrowZeroesPartialTail
//                                  │ Setattr.ShrinkReclaimsBlocks
//   lookup, read_dir_entries       │ Lookup.FindsAndMissing
//   readdir                        │ Readdir.ListsAndEmpty
//   add_dir_entry                  │ AddDirEntry.ReusesSlotAndRejectsLongName
//   make_node, create, mkdir       │ MakeNode.CreateMkdirIdempotentRejectsNonDir
//                                  │ MakeNode.RollsBackAllocatedInodeWhenLinkFails
//   unlink, remove_dir_entry,      │ Unlink.RemovesFreesIdempotentRejectsDir
//                                  │ Unlink.FreesIndirectAndDoubleIndirectBlocks
//     free_file_blocks             │   (free_file_blocks is also covered by Rmdir)
//   rmdir                          │ Rmdir.RemovesEmptyRejectsNonEmptyNonDir
#include <nanofs/fs.h>
#include <nanofs/inode.h>
#include <nanofs/block.h>
#include <gtest/gtest.h>
#include <cstring>
using namespace nanofs;

// Count the free data blocks: verifies free_file_blocks reclaims symmetrically (after delete == before create).
static int count_free_data_blocks(SimulatedBlockDevice& dev) {
  Superblock sb{};
  std::vector<std::byte> b(dev.block_size());
  EXPECT_TRUE(dev.read_block(0, b).has_value());
  std::memcpy(&sb, b.data(), sizeof(sb));
  BlockAllocator ba(dev, sb.block_bitmap_start, sb.block_bitmap_blocks,
                    sb.data_start, sb.num_blocks);
  int free = 0;
  for (block_id i = sb.data_start; i < sb.num_blocks; ++i)
    if (!ba.is_allocated(i).value()) ++free;
  return free;
}

static int count_free_inodes(SimulatedBlockDevice& dev, uint32_t num_inodes) {
  Superblock sb{};
  std::vector<std::byte> b(dev.block_size());
  EXPECT_TRUE(dev.read_block(0, b).has_value());
  std::memcpy(&sb, b.data(), sizeof(sb));
  BlockAllocator ia(dev, sb.inode_bitmap_start, sb.inode_bitmap_blocks,
                    ROOT_INODE, num_inodes + 1);
  int free = 0;
  for (inode_id i = ROOT_INODE; i <= num_inodes; ++i)
    if (!ia.is_allocated(i).value()) ++free;
  return free;
}

TEST(LogicalToPhysical, MapsTiersAndRejectsOverflow) {
  // One write-then-read spans all three tiers — direct → single-indirect →
  // double-indirect (every mapping path is exercised) — and the big file must
  // round-trip byte for byte. Then verify that an out-of-range logical block is
  // rejected by the bounds check (nothing gets allocated).
  //
  // bs must be >= sizeof(inode)=432 (NDIRECT=100), so the smallest usable block
  // size is 512 => nind=128:
  //   [0..99] direct · [100..227] single-indirect · [228..] double-indirect.
  // A file of 512*230+5 bytes puts its last byte in logical block 230 (>=228),
  // so all three tiers are covered.
  SimulatedBlockDevice dev(512, 8192);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  auto id = im.allocate_inode(INODE_FILE).value();

  std::string big(512 * 230 + 5, 'x');
  for (size_t i = 0; i < big.size(); ++i) big[i] = char('a' + (i % 26));  // every byte differs, to catch shifted or crossed blocks
  auto w = fs.write_file(id, 0, big);
  ASSERT_TRUE(w.has_value());
  EXPECT_EQ(*w, big.size());
  auto full = fs.read_file(id, 0, (uint32_t)big.size());
  ASSERT_TRUE(full.has_value());
  EXPECT_EQ(*full, big);                             // all three tiers match byte for byte
  auto tail = fs.read_file(id, 512 * 229, 100);      // offset lands in logical block 229, in the double-indirect tier
  ASSERT_TRUE(tail.has_value());
  EXPECT_EQ(*tail, big.substr(512 * 229, 100));

  // Out of range: the file can address 100 + 128 + 128*128 = 16612 logical
  // blocks (indices 0..16611).
  auto over = fs.write_file(id, 16612u * 512, "x");  // logical block 16612 is out of range (the check fires first; nothing is allocated)
  ASSERT_FALSE(over.has_value());
  EXPECT_EQ(over.error(), Error::Invalid);
}

TEST(ReadBytes, TruncatesAndZeroFillsHoles) {
  SimulatedBlockDevice dev(4096, 64);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);

  // Truncation: a read past size only returns bytes up to size; off >= size
  // returns an empty string.
  auto f = im.allocate_inode(INODE_FILE).value();
  ASSERT_TRUE(fs.write_file(f, 0, "hello").has_value());
  EXPECT_EQ(fs.read_file(f, 0, 100).value(), "hello");  // only 5 bytes come back
  EXPECT_EQ(fs.read_file(f, 10, 5).value(), "");         // off >= size

  // Hole: write only at a high offset, so the lower logical block was never
  // allocated => it reads back as all zeros.
  auto g = im.allocate_inode(INODE_FILE).value();
  ASSERT_TRUE(fs.write_file(g, 5000, "z").has_value());  // allocates block 1; block 0 is a hole
  EXPECT_EQ(fs.read_file(g, 0, 10).value(), std::string(10, '\0'));

  // Holes in the indirect tiers: the bs=4096 hole above only lands in the
  // direct tier. Here bs=512 (the single-indirect tier starts at logical block
  // 100, the double-indirect tier at 228) and we write just one high block in
  // the double-indirect tier, so the single-indirect pointer and some
  // second-level table entries were never allocated. Reading them must
  // recognize "indirect block / table entry pointer == 0" and return zeros,
  // instead of calling read_block(0) and getting superblock garbage. All three
  // hole kinds are hit: a direct hole / a missing single-indirect block / a
  // missing leaf pointer inside an allocated second-level table.
  SimulatedBlockDevice dev2(512, 8192);
  InodeManager im2(dev2, 16);
  ASSERT_TRUE(im2.format().has_value());
  FsLayer fs2(im2, dev2);
  auto h = im2.allocate_inode(INODE_FILE).value();
  ASSERT_TRUE(fs2.write_file(h, 512u * 300, "z").has_value());  // logical block 300, in the double-indirect tier
  EXPECT_EQ(fs2.read_file(h, 0, 10).value(), std::string(10, '\0'));           // direct hole
  EXPECT_EQ(fs2.read_file(h, 512u * 150, 10).value(), std::string(10, '\0'));  // single-indirect block pointer == 0
  EXPECT_EQ(fs2.read_file(h, 512u * 250, 10).value(), std::string(10, '\0'));  // leaf pointer inside the second-level table == 0
}

TEST(WriteBytes, GrowsAndRewritesPartialBlock) {
  SimulatedBlockDevice dev(4096, 64);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  auto f = im.allocate_inode(INODE_FILE).value();

  // Growth: a write extends size to off+len.
  ASSERT_TRUE(fs.write_file(f, 0, "hello world").has_value());
  EXPECT_EQ(fs.getattr(f)->size, 11u);

  // Partial-block overwrite (read-modify-write): a write covering part of a
  // block must read the old contents back first, change only the middle, and
  // leave the neighboring bytes alone.
  ASSERT_TRUE(fs.write_file(f, 6, "there").has_value());
  EXPECT_EQ(fs.read_file(f, 0, 11).value(), "hello there");
}

TEST(ReadFile, RejectsDirAndOverflow) {
  SimulatedBlockDevice dev(4096, 64);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  EXPECT_EQ(fs.read_file(ROOT_INODE, 0, 4).error(), Error::IsDir);   // reading a directory is rejected
  auto id = im.allocate_inode(INODE_FILE).value();
  EXPECT_EQ(fs.read_file(id, 0xFFFFFFF0u, 100).error(), Error::Invalid);  // off+len overflows u32
}

TEST(WriteFile, RejectsDirAndOverflow) {
  SimulatedBlockDevice dev(4096, 64);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  EXPECT_EQ(fs.write_file(ROOT_INODE, 0, "x").error(), Error::IsDir);  // writing a directory is rejected
  auto id = im.allocate_inode(INODE_FILE).value();
  EXPECT_EQ(fs.write_file(id, 0xFFFFFFF0u, std::string(16, 'x')).error(),
            Error::Invalid);                                          // off+len overflows u32
}

TEST(Getattr, ReportsTypeAndSize) {
  SimulatedBlockDevice dev(4096, 64);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  auto f = fs.create(ROOT_INODE, "f").value();
  ASSERT_TRUE(fs.write_file(f, 0, "abcd").has_value());
  auto a = fs.getattr(f);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->inode_id, f);
  EXPECT_FALSE(a->is_dir);
  EXPECT_EQ(a->size, 4u);
  auto root = fs.getattr(ROOT_INODE);
  ASSERT_TRUE(root.has_value());
  EXPECT_EQ(root->inode_id, ROOT_INODE);
  EXPECT_TRUE(root->is_dir);   // a directory reports is_dir==true
}

TEST(Setattr, ShrinkGrowIdempotent) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  auto f = fs.create(ROOT_INODE, "f").value();
  ASSERT_TRUE(fs.write_file(f, 0, std::string(10000, 'a')).has_value());
  EXPECT_EQ(fs.setattr(f, 3)->size, 3u);
  EXPECT_EQ(fs.read_file(f, 0, 100).value(), "aaa");
  EXPECT_EQ(fs.setattr(f, 3)->size, 3u);         // idempotent
}

TEST(Setattr, ShrinkGrowDoesNotAlias) {
  SimulatedBlockDevice dev(512, 8192);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  auto a = fs.create(ROOT_INODE, "a").value();
  auto b = fs.create(ROOT_INODE, "b").value();
  ASSERT_TRUE(fs.write_file(a, 0, std::string(512 * 230, 'A')).has_value());  // reaches into double-indirect
  ASSERT_TRUE(fs.setattr(a, 512).has_value());                               // shrink -> frees tail blocks, clears pointers
  ASSERT_TRUE(fs.write_file(b, 0, std::string(512 * 230, 'B')).has_value());  // b takes over the freed blocks
  ASSERT_TRUE(fs.write_file(a, 512 * 229, std::string(512, 'A')).has_value()); // a grows back into the truncated range
  EXPECT_EQ(fs.read_file(b, 0, 512 * 230).value(),
            std::string(512 * 230, 'B'));   // b must be intact (the shrink left no stale pointers into its blocks)
}

TEST(Setattr, GrowZeroesPartialTail) {
  // Shrink to the middle of a block, then grow back: the re-exposed tail of
  // that block must read back as zeros, not as leftover data from before the
  // shrink (POSIX ftruncate: a grown range reads as zeros).
  SimulatedBlockDevice dev(512, 256);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  auto f = fs.create(ROOT_INODE, "f").value();
  ASSERT_TRUE(fs.write_file(f, 0, std::string(1000, 'a')).has_value());  // block 0 is all 'a'
  ASSERT_TRUE(fs.setattr(f, 300).has_value());                           // shrink to the middle of block 0
  ASSERT_TRUE(fs.setattr(f, 800).has_value());                           // grow back past 300
  std::string want = std::string(300, 'a') + std::string(500, '\0');
  EXPECT_EQ(fs.read_file(f, 0, 800).value(), want);
}

TEST(Setattr, ShrinkReclaimsBlocks) {
  // A shrink must actually hand the dropped tail back to the allocator, and
  // get every tier boundary right: the indirect block at the boundary itself
  // stays and its tail pointer entries are cleared, while everything past it is
  // reclaimed as a whole subtree, structural blocks included. The first three
  // properties only check data read-back — that would pass even with no freeing
  // at all — so they cannot catch reclaim bugs. Instead, pin down the exact
  // usage at each level by counting free blocks.
  // bs=512 => ppb=128, NDIRECT=100: direct [0,100) · single [100,228) ·
  // double [228,..), 128 blocks per second-level table. Usage = data blocks +
  // structural blocks (single-indirect block? + double root? + number of
  // second-level tables).
  SimulatedBlockDevice dev(512, 8192);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  auto f = fs.create(ROOT_INODE, "f").value();
  const int base = count_free_data_blocks(dev);                    // baseline: empty file
  auto used = [&] { return base - count_free_data_blocks(dev); };  // blocks f currently uses

  // 361 blocks: reaches into the double tier's second second-level table.
  // Usage = 361 data + single + double root + 2 tables = 365.
  ASSERT_TRUE(fs.write_file(f, 0, std::string(512 * 361, 'A')).has_value());
  EXPECT_EQ(used(), 365);

  // Shrink to 300 blocks (inside the double tier; the boundary falls in the
  // first second-level table): keeps 300 data + single + double root + 1 table = 303.
  ASSERT_TRUE(fs.setattr(f, 512 * 300).has_value());
  EXPECT_EQ(used(), 303);
  EXPECT_EQ(fs.read_file(f, 512 * 299, 512).value(),
            std::string(512, 'A'));                        // the kept boundary block (last entry in the table) was not wrongly cleared

  // Shrink to 150 blocks (inside the single tier; the boundary falls in the
  // single-indirect block): keeps 150 data + the single-indirect block = 151
  // (the whole double-indirect tree is reclaimed).
  ASSERT_TRUE(fs.setattr(f, 512 * 150).has_value());
  EXPECT_EQ(used(), 151);

  // Shrink to 50 blocks (direct tier): keeps 50 data blocks and no indirect
  // structure = 50 (the single-indirect block is reclaimed too).
  ASSERT_TRUE(fs.setattr(f, 512 * 50).has_value());
  EXPECT_EQ(used(), 50);

  // Shrink to 0: fully back to the baseline.
  ASSERT_TRUE(fs.setattr(f, 0).has_value());
  EXPECT_EQ(used(), 0);
}

TEST(Lookup, FindsAndMissing) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  auto a = fs.create(ROOT_INODE, "a.txt").value();
  EXPECT_EQ(fs.lookup(ROOT_INODE, "a.txt").value(), a);              // found by name
  EXPECT_EQ(fs.lookup(ROOT_INODE, "nope").error(), Error::NotFound);  // does not exist
  EXPECT_EQ(fs.lookup(a, "x").error(), Error::NotDir);                // parent is not a directory
}

TEST(Readdir, ListsAndEmpty) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  EXPECT_TRUE(fs.readdir(ROOT_INODE)->empty());       // empty directory
  ASSERT_TRUE(fs.create(ROOT_INODE, "a").has_value());
  ASSERT_TRUE(fs.mkdir(ROOT_INODE, "b").has_value());
  EXPECT_EQ(fs.readdir(ROOT_INODE)->size(), 2u);      // lists both entries
  auto f = fs.create(ROOT_INODE, "file").value();
  EXPECT_EQ(fs.readdir(f).error(), Error::NotDir);    // target is not a directory

  // Spanning blocks: DirEntry=64B, bs=512 => 8 entries per block. Creating 10
  // entries makes the directory span 2 data blocks; readdir (via
  // read_dir_entries) must parse all the blocks and return all 10 entries, not
  // just the 8 in the first block.
  SimulatedBlockDevice dev2(512, 512);
  InodeManager im2(dev2, 64);
  ASSERT_TRUE(im2.format().has_value());
  FsLayer fs2(im2, dev2);
  for (int i = 0; i < 10; ++i)
    ASSERT_TRUE(fs2.create(ROOT_INODE, "f" + std::to_string(i)).has_value());
  EXPECT_GT(fs2.getattr(ROOT_INODE)->size, 512u);     // confirms it really spans blocks
  EXPECT_EQ(fs2.readdir(ROOT_INODE)->size(), 10u);
}

TEST(AddDirEntry, ReusesSlotAndRejectsLongName) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);

  // Reusing an empty slot: after deleting a, creating c should reuse a's slot
  // and the directory should not grow (reuse a free slot before appending).
  ASSERT_TRUE(fs.create(ROOT_INODE, "a").has_value());
  ASSERT_TRUE(fs.create(ROOT_INODE, "b").has_value());
  uint32_t size_two = fs.getattr(ROOT_INODE)->size;   // directory size with 2 entries
  ASSERT_TRUE(fs.unlink(ROOT_INODE, "a").has_value());
  ASSERT_TRUE(fs.create(ROOT_INODE, "c").has_value());
  EXPECT_EQ(fs.getattr(ROOT_INODE)->size, size_two);  // did not grow => the slot was reused
  EXPECT_EQ(fs.readdir(ROOT_INODE)->size(), 2u);      // b, c

  // A name that is too long (>= NANOFS_NAME_MAX = 60) is rejected.
  EXPECT_TRUE(fs.create(ROOT_INODE, std::string(59, 'x')).has_value());
  EXPECT_EQ(fs.create(ROOT_INODE, std::string(60, 'y')).error(), Error::Invalid);
}

TEST(MakeNode, RollsBackAllocatedInodeWhenLinkFails) {
  SimulatedBlockDevice dev(4096, 256);
  constexpr uint32_t kNumInodes = 64;
  InodeManager im(dev, kNumInodes);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);

  const int free_blocks = count_free_data_blocks(dev);
  const int free_inodes = count_free_inodes(dev, kNumInodes);
  auto bad = fs.create(ROOT_INODE, std::string(NANOFS_NAME_MAX, 'x'));
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), Error::Invalid);
  EXPECT_EQ(count_free_data_blocks(dev), free_blocks);
  EXPECT_EQ(count_free_inodes(dev, kNumInodes), free_inodes);

  auto ok = fs.create(ROOT_INODE, "ok");
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(*ok, 2u);  // after the rollback, the first free inode should still be reused
}

TEST(MakeNode, CreateMkdirIdempotentRejectsNonDir) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);

  auto a = fs.create(ROOT_INODE, "a.txt");
  auto d = fs.mkdir(ROOT_INODE, "sub");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(d.has_value());
  EXPECT_TRUE(fs.getattr(*d)->is_dir);

  // Idempotent: repeating create/mkdir returns the existing inode and adds no
  // new directory entry.
  EXPECT_EQ(fs.create(ROOT_INODE, "a.txt").value(), *a);
  EXPECT_EQ(fs.mkdir(ROOT_INODE, "sub").value(), *d);
  EXPECT_EQ(fs.readdir(ROOT_INODE)->size(), 2u);

  // Nesting: creation also works under a non-root directory (make_node uses
  // the parent it is given); write-then-read round-trips.
  auto inner = fs.create(*d, "inner");
  ASSERT_TRUE(inner.has_value());
  ASSERT_TRUE(fs.write_file(*inner, 0, "hi").has_value());
  EXPECT_EQ(fs.read_file(*inner, 0, 2).value(), "hi");

  // Parent is not a directory => NotDir.
  EXPECT_EQ(fs.create(*a, "x").error(), Error::NotDir);
}

TEST(Unlink, RemovesFreesIdempotentRejectsDir) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);

  // The root directory allocates a data block the first time it gets an entry,
  // and remove_dir_entry only clears the slot without shrinking (see
  // AddDirEntry), so once allocated that block stays in use forever. Create and
  // delete a warmup entry first so the root directory already holds the block,
  // and this one-time growth does not leak into the reclaim-symmetry checks
  // below.
  ASSERT_TRUE(fs.create(ROOT_INODE, "warmup").has_value());
  ASSERT_TRUE(fs.unlink(ROOT_INODE, "warmup").has_value());
  int base = count_free_data_blocks(dev);

  auto f = fs.create(ROOT_INODE, "f").value();
  ASSERT_TRUE(fs.write_file(f, 0, std::string(4096 * 3, 'q')).has_value());  // takes several data blocks
  EXPECT_LT(count_free_data_blocks(dev), base);
  ASSERT_TRUE(fs.unlink(ROOT_INODE, "f").has_value());
  EXPECT_EQ(fs.lookup(ROOT_INODE, "f").error(), Error::NotFound);
  EXPECT_EQ(count_free_data_blocks(dev), base);          // content blocks and the inode's block are both reclaimed
  EXPECT_TRUE(fs.unlink(ROOT_INODE, "f").has_value());   // does not exist -> ok (idempotent)

  // Rejecting directories: unlink on a directory must fail; otherwise it would
  // free the directory's blocks and inode and orphan the children.
  auto dir = fs.mkdir(ROOT_INODE, "d").value();
  ASSERT_TRUE(fs.create(dir, "child").has_value());
  EXPECT_EQ(fs.unlink(ROOT_INODE, "d").error(), Error::IsDir);
  EXPECT_TRUE(fs.lookup(ROOT_INODE, "d").has_value());   // the directory is still there
  EXPECT_TRUE(fs.lookup(dir, "child").has_value());      // child was not orphaned
}

TEST(Unlink, FreesIndirectAndDoubleIndirectBlocks) {
  SimulatedBlockDevice dev(512, 8192);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);

  ASSERT_TRUE(fs.create(ROOT_INODE, "warmup").has_value());
  ASSERT_TRUE(fs.unlink(ROOT_INODE, "warmup").has_value());
  const int base = count_free_data_blocks(dev);

  auto f = fs.create(ROOT_INODE, "big").value();
  ASSERT_TRUE(fs.write_file(f, 0, std::string(512 * 361, 'q')).has_value());
  // 361 content blocks + the single-indirect block + the double-indirect root
  // + 2 second-level tables + the file's inode block.
  EXPECT_EQ(base - count_free_data_blocks(dev), 366);
  ASSERT_TRUE(fs.unlink(ROOT_INODE, "big").has_value());
  EXPECT_EQ(fs.lookup(ROOT_INODE, "big").error(), Error::NotFound);
  EXPECT_EQ(count_free_data_blocks(dev), base);
}

TEST(Rmdir, RemovesEmptyRejectsNonEmptyNonDir) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);

  // Reclaim: after a warmup stabilizes the root directory's block, create
  // directory d plus a child to take some blocks; after rmdir every block is
  // reclaimed, and rmdir is idempotent.
  ASSERT_TRUE(fs.mkdir(ROOT_INODE, "warmup").has_value());
  ASSERT_TRUE(fs.rmdir(ROOT_INODE, "warmup").has_value());
  int base = count_free_data_blocks(dev);
  auto d = fs.mkdir(ROOT_INODE, "d").value();
  ASSERT_TRUE(fs.create(d, "x").has_value());            // makes d allocate a data block
  EXPECT_LT(count_free_data_blocks(dev), base);
  ASSERT_TRUE(fs.unlink(d, "x").has_value());            // empty out d
  ASSERT_TRUE(fs.rmdir(ROOT_INODE, "d").has_value());
  EXPECT_EQ(fs.lookup(ROOT_INODE, "d").error(), Error::NotFound);
  EXPECT_EQ(count_free_data_blocks(dev), base);          // directory data block and inode block are both reclaimed
  EXPECT_TRUE(fs.rmdir(ROOT_INODE, "d").has_value());    // does not exist -> ok (idempotent)

  // Rejections: a non-empty directory -> NotEmpty; a file target -> NotDir.
  auto d2 = fs.mkdir(ROOT_INODE, "d2").value();
  ASSERT_TRUE(fs.create(d2, "child").has_value());
  EXPECT_EQ(fs.rmdir(ROOT_INODE, "d2").error(), Error::NotEmpty);
  EXPECT_TRUE(fs.lookup(d2, "child").has_value());       // not damaged
  ASSERT_TRUE(fs.create(ROOT_INODE, "file").has_value());
  EXPECT_EQ(fs.rmdir(ROOT_INODE, "file").error(), Error::NotDir);
}
