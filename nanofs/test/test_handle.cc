#include <nanofs/handle.h>
#include <nanofs/inode.h>
#include <nanofs/block.h>
#include <gtest/gtest.h>
using namespace nanofs;

TEST(ResolveHandle, MatchingGenerationReturnsId) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  auto id = im.allocate_inode(INODE_FILE);
  ASSERT_TRUE(id.has_value());
  auto g = im.generation(*id);
  ASSERT_TRUE(g.has_value());
  auto r = resolve_handle(im, FileHandle{*id, *g});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, *id);
}

TEST(ResolveHandle, StaleGenerationRejected) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  auto id = im.allocate_inode(INODE_FILE);
  ASSERT_TRUE(id.has_value());
  auto g = im.generation(*id);
  ASSERT_TRUE(g.has_value());
  auto r = resolve_handle(im, FileHandle{*id, *g + 1});   // generation mismatch
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), Error::Stale);
}

TEST(ResolveHandle, FreedInodeIsStale) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  auto id = im.allocate_inode(INODE_FILE);
  ASSERT_TRUE(id.has_value());
  auto g = im.generation(*id);
  ASSERT_TRUE(g.has_value());
  ASSERT_TRUE(im.free_inode(*id).has_value());
  auto r = resolve_handle(im, FileHandle{*id, *g});   // generation still matches, but the id is freed
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), Error::Stale);
}

TEST(ResolveHandle, InvalidInodeNumberIsStale) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  for (auto id : {0u, 65u}) {
    auto result = resolve_handle(im, FileHandle{id, 1});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Error::Stale);
  }
}
