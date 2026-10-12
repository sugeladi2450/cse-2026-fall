#include <nanofs/block.h>
#include <nanofs/inode.h>
#include <nanofs/journal.h>
#include <nanofs/locked_fs.h>

#include <string>

#include <gtest/gtest.h>

using namespace nanofs;

namespace {
// Seed through the completed transaction layer so each case isolates one
// LockedFs operation instead of depending on LockedFs::create first.
struct Fs {
  static constexpr uint32_t kInodes = 32;
  SimulatedBlockDevice image{4096, 128}, backing{4096, 128};
  JournaledBlockDevice journal{image, backing, 96};
  InodeManager inodes{journal, kInodes};
  TxFs seed{inodes, journal};
  LockedFs locked{inodes, journal, kInodes};

  Fs() {
    EXPECT_TRUE(inodes.format());
    EXPECT_TRUE(journal.checkpoint());
  }
};
}  // namespace

TEST(LockedFsBasic, LookupExistingAndMissing) {
  Fs t;
  auto id = t.seed.create(ROOT_INODE, "file");
  ASSERT_TRUE(id);
  auto found = t.locked.lookup(ROOT_INODE, "file");
  ASSERT_TRUE(found);
  EXPECT_EQ(*found, *id);
  auto missing = t.locked.lookup(ROOT_INODE, "missing");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error(), Error::NotFound);
}

TEST(LockedFsBasic, GetattrReportsFileSize) {
  Fs t;
  auto id = t.seed.create(ROOT_INODE, "file");
  ASSERT_TRUE(id);
  ASSERT_TRUE(t.seed.write_file(*id, 0, "hello"));
  auto attr = t.locked.getattr(*id);
  ASSERT_TRUE(attr);
  EXPECT_EQ(attr->inode_id, *id);
  EXPECT_FALSE(attr->is_dir);
  EXPECT_EQ(attr->size, 5u);
}

TEST(LockedFsBasic, ReaddirListsSeededEntry) {
  Fs t;
  auto id = t.seed.create(ROOT_INODE, "file");
  ASSERT_TRUE(id);
  auto entries = t.locked.readdir(ROOT_INODE);
  ASSERT_TRUE(entries);
  ASSERT_EQ(entries->size(), 1u);
  EXPECT_EQ(std::string((*entries)[0].name), "file");
  EXPECT_EQ((*entries)[0].inode_id, *id);
}

TEST(LockedFsBasic, ReadFileReturnsSeededBytes) {
  Fs t;
  auto id = t.seed.create(ROOT_INODE, "file");
  ASSERT_TRUE(id);
  ASSERT_TRUE(t.seed.write_file(*id, 0, "abcdef"));
  auto data = t.locked.read_file(*id, 2, 3);
  ASSERT_TRUE(data);
  EXPECT_EQ(*data, "cde");
}

TEST(LockedFsBasic, WriteFilePersistsBytes) {
  Fs t;
  auto id = t.seed.create(ROOT_INODE, "file");
  ASSERT_TRUE(id);
  auto written = t.locked.write_file(*id, 0, "hello");
  ASSERT_TRUE(written);
  EXPECT_EQ(*written, 5u);
  auto data = t.seed.read_file(*id, 0, 5);
  ASSERT_TRUE(data);
  EXPECT_EQ(*data, "hello");
}

TEST(LockedFsBasic, SetattrTruncatesFile) {
  Fs t;
  auto id = t.seed.create(ROOT_INODE, "file");
  ASSERT_TRUE(id);
  ASSERT_TRUE(t.seed.write_file(*id, 0, "abcdef"));
  auto attr = t.locked.setattr(*id, 2);
  ASSERT_TRUE(attr);
  EXPECT_EQ(attr->size, 2u);
  auto data = t.seed.read_file(*id, 0, 6);
  ASSERT_TRUE(data);
  EXPECT_EQ(*data, "ab");
}

TEST(LockedFsBasic, CreateAddsFile) {
  Fs t;
  auto id = t.locked.create(ROOT_INODE, "file");
  ASSERT_TRUE(id);
  auto found = t.seed.lookup(ROOT_INODE, "file");
  ASSERT_TRUE(found);
  EXPECT_EQ(*found, *id);
}

TEST(LockedFsBasic, MkdirAddsDirectory) {
  Fs t;
  auto id = t.locked.mkdir(ROOT_INODE, "dir");
  ASSERT_TRUE(id);
  auto attr = t.seed.getattr(*id);
  ASSERT_TRUE(attr);
  EXPECT_TRUE(attr->is_dir);
  EXPECT_TRUE(t.seed.lookup(ROOT_INODE, "dir"));
}

TEST(LockedFsBasic, UnlinkRemovesSeededFile) {
  Fs t;
  ASSERT_TRUE(t.seed.create(ROOT_INODE, "file"));
  ASSERT_TRUE(t.locked.unlink(ROOT_INODE, "file"));
  auto found = t.seed.lookup(ROOT_INODE, "file");
  EXPECT_FALSE(found);
  if (!found) EXPECT_EQ(found.error(), Error::NotFound);
}

TEST(LockedFsBasic, RmdirRemovesSeededDirectory) {
  Fs t;
  ASSERT_TRUE(t.seed.mkdir(ROOT_INODE, "dir"));
  ASSERT_TRUE(t.locked.rmdir(ROOT_INODE, "dir"));
  auto found = t.seed.lookup(ROOT_INODE, "dir");
  EXPECT_FALSE(found);
  if (!found) EXPECT_EQ(found.error(), Error::NotFound);
}
