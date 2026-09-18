#include <nanofs/fuse_adapter.h>
#include <nanofs/fs.h>
#include <nanofs/local_fs.h>
#include <nanofs/inode.h>
#include <nanofs/block.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <string>

using namespace nanofs;

TEST(FusePathAdapter, PathOpsReachFsLayer) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  LocalFs backend(fs, im, false);
  FusePathAdapter adapter(backend);

  ASSERT_TRUE(adapter.mkdir("/dir").has_value());
  auto created = adapter.create("/dir/file.txt");
  ASSERT_TRUE(created.has_value());
  auto written = adapter.write("/dir/file.txt", 0, "hello fuse");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(*written, 10u);

  auto attr = adapter.getattr("/dir/file.txt");
  ASSERT_TRUE(attr.has_value());
  EXPECT_FALSE(attr->is_dir);
  EXPECT_EQ(attr->size, 10u);

  auto data = adapter.read("/dir/file.txt", 6, 4);
  ASSERT_TRUE(data.has_value());
  EXPECT_EQ(*data, "fuse");

  auto entries = adapter.readdir("/dir");
  ASSERT_TRUE(entries.has_value());
  ASSERT_EQ(entries->size(), 1u);
  EXPECT_EQ((*entries)[0].name, "file.txt");
  EXPECT_EQ((*entries)[0].inode_id, created->inode_id);
}

TEST(FusePathAdapter, RmdirRemovesEmptyDirRejectsNonEmpty) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  LocalFs backend(fs, im, false);
  FusePathAdapter adapter(backend);

  ASSERT_TRUE(adapter.mkdir("/dir").has_value());
  ASSERT_TRUE(adapter.create("/dir/file.txt").has_value());
  EXPECT_EQ(adapter.rmdir("/dir").error(), Error::NotEmpty);   // non-empty directory is rejected
  ASSERT_TRUE(adapter.unlink("/dir/file.txt").has_value());
  ASSERT_TRUE(adapter.rmdir("/dir").has_value());              // removable once emptied
  EXPECT_EQ(adapter.getattr("/dir").error(), Error::NotFound);
}

TEST(FusePathAdapter, MissingParentIsNotFound) {
  SimulatedBlockDevice dev(4096, 64);
  InodeManager im(dev, 16);
  ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  LocalFs backend(fs, im, false);
  FusePathAdapter adapter(backend);

  auto created = adapter.create("/missing/file.txt");
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error(), Error::NotFound);
}

TEST(FuseMetadata, UsesMountOwnerForKernelPermissionChecks) {
  auto meta = fuse_metadata(Attr{ROOT_INODE, true, 0}, 501, 20);

  EXPECT_EQ(meta.uid, 501u);
  EXPECT_EQ(meta.gid, 20u);
  EXPECT_EQ(meta.mode, static_cast<uint32_t>(S_IFDIR | 0755));
  EXPECT_EQ(meta.nlink, 2u);
}
