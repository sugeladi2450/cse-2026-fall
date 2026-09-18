#include <nanofs/block.h>
#include <gtest/gtest.h>
#include <vector>
#include <cstddef>
#include <cstdio>
#include <string>
using namespace nanofs;

static std::vector<std::byte> bytes(size_t n, unsigned char v) {
  return std::vector<std::byte>(n, std::byte{v});
}

TEST(SimBlockDeviceMem, WriteThenReadRoundTrips) {
  SimulatedBlockDevice dev(64, 8);      // 64B blocks, 8 blocks
  EXPECT_EQ(dev.block_size(), 64u);
  EXPECT_EQ(dev.num_blocks(), 8u);
  auto in = bytes(64, 0xAB);
  ASSERT_TRUE(dev.write_block(3, in).has_value());
  std::vector<std::byte> out(64);
  ASSERT_TRUE(dev.read_block(3, out).has_value());
  EXPECT_EQ(out, in);
}

TEST(SimBlockDeviceMem, ZeroBlockClears) {
  SimulatedBlockDevice dev(64, 8);
  ASSERT_TRUE(dev.write_block(1, bytes(64, 0xFF)).has_value());
  ASSERT_TRUE(dev.zero_block(1).has_value());
  std::vector<std::byte> out(64);
  ASSERT_TRUE(dev.read_block(1, out).has_value());
  EXPECT_EQ(out, bytes(64, 0x00));
}

TEST(SimBlockDeviceMem, OutOfRangeBlockIsIoError) {
  SimulatedBlockDevice dev(64, 8);
  auto r = dev.write_block(8, bytes(64, 0x01));   // valid ids are 0..7
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), Error::IoError);
}

TEST(SimBlockDeviceMem, WrongBufferSizeIsInvalid) {
  SimulatedBlockDevice dev(64, 8);
  auto r = dev.write_block(0, bytes(32, 0x01));   // 32 != block_size 64
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), Error::Invalid);
}

TEST(SimBlockDeviceMem, ReadBlockWrongBufferSizeIsInvalid) {
  SimulatedBlockDevice dev(64, 8);
  std::vector<std::byte> small(32);
  auto r = dev.read_block(0, small);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), Error::Invalid);
}

TEST(SimBlockDeviceMem, ReadBlockOutOfRangeIsIoError) {
  SimulatedBlockDevice dev(64, 8);
  std::vector<std::byte> buf(64);
  auto r = dev.read_block(8, buf);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), Error::IoError);
}

TEST(SimBlockDeviceMem, ZeroBlockOutOfRangeIsIoError) {
  SimulatedBlockDevice dev(64, 8);
  auto r = dev.zero_block(8);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), Error::IoError);
}

TEST(SimBlockDeviceFile, PersistsAcrossReopen) {
  std::string path = "/tmp/nanofs_p1_dev.img";
  std::remove(path.c_str());
  {
    auto d = SimulatedBlockDevice::open_file(path, 64, 8);
    ASSERT_TRUE(d.has_value());
    ASSERT_TRUE((*d)->write_block(5, std::vector<std::byte>(64, std::byte{0x7E})).has_value());
  }  // destructor -> closes the file
  auto d2 = SimulatedBlockDevice::open_file(path, 64, 8);
  ASSERT_TRUE(d2.has_value());
  std::vector<std::byte> out(64);
  ASSERT_TRUE((*d2)->read_block(5, out).has_value());
  EXPECT_EQ(out, std::vector<std::byte>(64, std::byte{0x7E}));
  std::remove(path.c_str());
}

TEST(SimBlockDeviceFile, EnforcesChecksOnFileBackend) {
  std::string path = "/tmp/nanofs_p1_dev_checks.img";
  std::remove(path.c_str());
  auto d = SimulatedBlockDevice::open_file(path, 64, 8);
  ASSERT_TRUE(d.has_value());
  std::vector<std::byte> wrong(32), ok(64);
  EXPECT_EQ(d.value()->write_block(0, wrong).error(), Error::Invalid);   // wrong size
  EXPECT_EQ(d.value()->read_block(8, ok).error(), Error::IoError);       // out of range
  EXPECT_EQ(d.value()->zero_block(8).error(), Error::IoError);           // out of range
  std::remove(path.c_str());
}

TEST(BlockAllocator, AllocatesFromManagedRangeAndReclaims) {
  SimulatedBlockDevice dev(64, 32);
  // The bitmap lives in block 1 (one block is enough for 32 bits); it manages
  // the data range [4, 32).
  BlockAllocator alloc(dev, /*bitmap_start=*/1, /*bitmap_blocks=*/1,
                       /*managed_begin=*/4, /*managed_end=*/32);
  ASSERT_TRUE(alloc.format().has_value());        // clears the bitmap
  auto a = alloc.allocate(); ASSERT_TRUE(a.has_value());
  auto b = alloc.allocate(); ASSERT_TRUE(b.has_value());
  EXPECT_EQ(*a, 4u);
  EXPECT_EQ(*b, 5u);
  ASSERT_TRUE(alloc.free(*a).has_value());
  auto c = alloc.allocate(); ASSERT_TRUE(c.has_value());
  EXPECT_EQ(*c, 4u);                               // after freeing, the next allocate gets 4 back
}

TEST(BlockAllocator, ExhaustionIsNoSpace) {
  SimulatedBlockDevice dev(64, 8);
  BlockAllocator alloc(dev, 1, 1, 6, 8);           // only blocks 6 and 7 can be allocated
  ASSERT_TRUE(alloc.format().has_value());
  ASSERT_TRUE(alloc.allocate().has_value());
  ASSERT_TRUE(alloc.allocate().has_value());
  auto r = alloc.allocate();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), Error::NoSpace);
}

TEST(BlockAllocator, BitmapPersistsOnDevice) {
  SimulatedBlockDevice dev(64, 32);
  { BlockAllocator a(dev, 1, 1, 4, 32); ASSERT_TRUE(a.format().has_value());
    ASSERT_TRUE(a.allocate().has_value()); }       // allocates block 4, writes the bitmap back
  BlockAllocator a2(dev, 1, 1, 4, 32);             // no format; reads the existing bitmap
  EXPECT_TRUE(a2.is_allocated(4).value());
  EXPECT_FALSE(a2.is_allocated(5).value());
}

TEST(BlockAllocator, MarkUsedForcesBitSet) {
  SimulatedBlockDevice dev(64, 32);
  BlockAllocator a(dev, 1, 1, 4, 32);
  ASSERT_TRUE(a.format().has_value());
  ASSERT_TRUE(a.mark_used(2).has_value());          // a metadata block (outside the managed range)
  EXPECT_TRUE(a.is_allocated(2).value());
  EXPECT_FALSE(a.is_allocated(4).value());          // the managed range is still free
}

TEST(BlockAllocator, MultiBlockBitmapAddressing) {
  SimulatedBlockDevice dev(8, 200);   // 8B blocks => 64 bits per bitmap block
  BlockAllocator a(dev, /*bitmap_start=*/1, /*bitmap_blocks=*/2,
                   /*managed_begin=*/10, /*managed_end=*/128);
  ASSERT_TRUE(a.format().has_value());
  // block id 70 -> byte 8 -> second bitmap block; id 5 -> byte 0 -> first block.
  ASSERT_TRUE(a.mark_used(70).has_value());
  EXPECT_TRUE(a.is_allocated(70).value());
  EXPECT_FALSE(a.is_allocated(5).value());    // first bitmap block untouched
  EXPECT_FALSE(a.is_allocated(71).value());   // neighbor bit in same byte untouched
}

TEST(BlockVolatile, WriteVisibleBeforeFlushButNotDurableAfterCrash) {
  SimulatedBlockDevice dev(64, 8);
  std::vector<std::byte> w(64, std::byte{0xAB}), r(64);
  ASSERT_TRUE(dev.write_block(3, w).has_value());
  ASSERT_TRUE(dev.read_block(3, r).has_value());
  EXPECT_EQ(r, w);                         // readable even before flush (visible in the volatile layer)
  dev.reboot();                    // drops the unflushed writes
  std::vector<std::byte> r2(64);
  ASSERT_TRUE(dev.read_block(3, r2).has_value());
  EXPECT_EQ(r2, std::vector<std::byte>(64, std::byte{0}));  // after the crash it is back to the durable content (zeros)
}

// Power loss inside a flush: only the first k dirty blocks (block-id order) reach the
// medium; the flush fails, the rest are gone, and every device is dead until reboot.
TEST(BlockVolatile, CrashAtFlushKeepsOnlyFirstKBlocks) {
  SimulatedBlockDevice dev(64, 8);
  std::vector<std::byte> w(64, std::byte{0xEF}), zero(64, std::byte{0}), r(64);
  for (block_id id : {2, 5, 7}) ASSERT_TRUE(dev.write_block(id, w).has_value());
  dev.crash_at_flush(1, 2);
  EXPECT_FALSE(dev.flush().has_value());
  EXPECT_TRUE(dev.crashed());
  for (block_id id : {2, 5}) { ASSERT_TRUE(dev.read_block(id, r).has_value()); EXPECT_EQ(r, w); }
  ASSERT_TRUE(dev.read_block(7, r).has_value());
  EXPECT_EQ(r, zero);
  SimulatedBlockDevice other(64, 8);
  EXPECT_FALSE(dev.write_block(1, w).has_value());   // dead until reboot: nothing more lands
  EXPECT_FALSE(dev.flush().has_value());
  EXPECT_FALSE(other.write_block(1, w).has_value()); // power loss is machine-wide
  dev.reboot();                              // reboot
  ASSERT_TRUE(dev.write_block(1, w).has_value());
  EXPECT_TRUE(dev.flush().has_value());
}

TEST(BlockVolatile, FlushMakesWritesSurviveCrash) {
  SimulatedBlockDevice dev(64, 8);
  std::vector<std::byte> w(64, std::byte{0xCD});
  ASSERT_TRUE(dev.write_block(5, w).has_value());
  ASSERT_TRUE(dev.flush().has_value());
  dev.reboot();
  std::vector<std::byte> r(64);
  ASSERT_TRUE(dev.read_block(5, r).has_value());
  EXPECT_EQ(r, w);                          // flushed, so it survives the crash
}
