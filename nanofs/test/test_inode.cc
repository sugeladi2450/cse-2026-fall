#include <nanofs/inode.h>
#include <nanofs/block.h>
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
using namespace nanofs;

TEST(InodeStruct, IsTriviallyCopyableAllUint32) {
  static_assert(std::is_trivially_copyable_v<inode>);
  // 6 scalar uint32 fields + (NDIRECT+2) pointers, all uint32, no padding.
  EXPECT_EQ(sizeof(inode), sizeof(uint32_t) * (6 + NDIRECT + 2));
}

TEST(InodeManagerFormat, CreatesRootDirAndRejectsTinyBlock) {
  {
    SimulatedBlockDevice dev(4096, 64);
    InodeManager im(dev, /*num_inodes=*/32);
    ASSERT_TRUE(im.format().has_value());
    auto root = im.read_inode(ROOT_INODE);
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(root->type, INODE_DIR);
    EXPECT_EQ(root->nlink, 1u);
    // Block 0 (the block holding the superblock) should be marked used in the
    // block bitmap after format: read the superblock, rebuild a BlockAllocator
    // pointing at the same bitmap region, and query bit 0.
    std::vector<std::byte> buf(dev.block_size());
    ASSERT_TRUE(dev.read_block(0, buf).has_value());
    Superblock sb{};
    std::memcpy(&sb, buf.data(), sizeof(sb));
    BlockAllocator balloc(dev, sb.block_bitmap_start, sb.block_bitmap_blocks,
                          0, sb.data_start);
    auto used = balloc.is_allocated(0);
    ASSERT_TRUE(used.has_value());
    EXPECT_TRUE(*used);
  }
  {
    SimulatedBlockDevice small(16, 64);   // 16 < sizeof(inode)
    InodeManager im(small, 32);
    auto r = im.format();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), Error::Invalid);
  }
}

TEST(InodeManagerAlloc, AllocatesUniqueIdsAndRoundTrips) {
  SimulatedBlockDevice dev(4096, 128);
  InodeManager im(dev, 32);
  ASSERT_TRUE(im.format().has_value());
  auto a = im.allocate_inode(INODE_FILE); ASSERT_TRUE(a.has_value());
  auto b = im.allocate_inode(INODE_FILE); ASSERT_TRUE(b.has_value());
  EXPECT_NE(*a, *b);
  EXPECT_EQ(*a, 2u);
  // Write a size back, then read it out again.
  inode n = im.read_inode(*a).value();
  n.size = 1234;
  ASSERT_TRUE(im.write_inode(*a, n).has_value());
  EXPECT_EQ(im.read_inode(*a).value().size, 1234u);
  EXPECT_EQ(im.read_inode(*a).value().type, INODE_FILE);
}

TEST(InodeManagerAlloc, FreeReclaimsAndGetOnNilIsNotFound) {
  SimulatedBlockDevice dev(4096, 128);
  InodeManager im(dev, 32);
  ASSERT_TRUE(im.format().has_value());
  auto a = im.allocate_inode(INODE_FILE).value();
  auto g = im.generation(a).value();
  ASSERT_TRUE(im.free_inode(a).has_value());
  // A freed id no longer maps to a block: get / read_inode / write_inode all fail, so nobody
  // can write into the block that was just returned to the allocator.
  EXPECT_EQ(im.get(a).error(), Error::NotFound);
  EXPECT_EQ(im.read_inode(a).error(), Error::NotFound);
  EXPECT_EQ(im.write_inode(a, inode{}).error(), Error::NotFound);
  EXPECT_EQ(im.generation(a).value(), g);    // the generation survives the free
  EXPECT_EQ(im.free_inode(a).error(), Error::NotFound);   // double free
  auto miss = im.get(0);                     // nil
  ASSERT_FALSE(miss.has_value());
  EXPECT_EQ(miss.error(), Error::NotFound);
}

TEST(InodeManagerAlloc, AllocateInodeRollsBackInodeBitOnBlockExhaustion) {
  SimulatedBlockDevice dev(4096, 8);   // only 3 free data blocks after format
  InodeManager im(dev, 32);
  ASSERT_TRUE(im.format().has_value());
  // Each allocate_inode consumes one data block; exhaust the pool.
  std::vector<inode_id> ok;
  while (true) {
    auto r = im.allocate_inode(INODE_FILE);
    if (!r) { EXPECT_EQ(r.error(), Error::NoSpace); break; }
    ok.push_back(*r);
  }
  ASSERT_FALSE(ok.empty());
  inode_id would_be = ok.back() + 1;   // the id the failed attempt reserved
  // Without rollback, this id's inode-bitmap bit stays set (leaked); with the fix it's free.
  BlockAllocator ialloc(dev, /*bitmap_start=*/2, /*bitmap_blocks=*/1,
                        /*managed_begin=*/1, /*managed_end=*/33);
  EXPECT_FALSE(ialloc.is_allocated(would_be).value());
}

TEST(InodeManagerAlloc, AllocFreeBlockBitmapSymmetry) {
  SimulatedBlockDevice dev(4096, 64);
  InodeManager im(dev, 32);
  ASSERT_TRUE(im.format().has_value());
  auto count_free_data = [&]() {
    BlockAllocator b(dev, /*bitmap_start=*/3, /*bitmap_blocks=*/1,
                     /*managed_begin=*/4, /*managed_end=*/64);
    int free = 0;
    for (block_id i = 4; i < 64; ++i)
      if (!b.is_allocated(i).value()) ++free;
    return free;
  };
  const int baseline = count_free_data();
  for (int i = 0; i < 5; ++i) {
    auto id = im.allocate_inode(INODE_FILE);
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(im.free_inode(*id).has_value());
  }
  EXPECT_EQ(count_free_data(), baseline);   // no per-cycle struct-block leak
}

