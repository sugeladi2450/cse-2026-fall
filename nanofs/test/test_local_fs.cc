#include <nanofs/local_fs.h>
#include <nanofs/fs.h>
#include <nanofs/inode.h>
#include <nanofs/block.h>
#include <gtest/gtest.h>
using namespace nanofs;

namespace {
struct Env {
  SimulatedBlockDevice dev{4096, 256};
  InodeManager im{dev, 64};
  Env() { (void)im.format(); }
};
FileHandle root() { return FileHandle{ROOT_INODE, ROOT_GENERATION}; }
}  // namespace

TEST(LocalFs, CreateWriteReadRoundTrip) {
  Env e; FsLayer fs(e.im, e.dev); LocalFs lfs(fs, e.im);
  auto f = lfs.create(root(), "a.txt");
  ASSERT_TRUE(f.has_value());
  auto w = lfs.write_file(*f, 0, "hello");
  ASSERT_TRUE(w.has_value()); EXPECT_EQ(*w, 5u);
  auto r = lfs.read_file(*f, 0, 5);
  ASSERT_TRUE(r.has_value()); EXPECT_EQ(*r, "hello");
  auto lk = lfs.lookup(root(), "a.txt");
  ASSERT_TRUE(lk.has_value()); EXPECT_EQ(lk->id, f->id);
}

TEST(LocalFs, FreshHandleOk) {
  Env e; FsLayer fs(e.im, e.dev); LocalFs lfs(fs, e.im);
  auto d = lfs.mkdir(root(), "d");
  ASSERT_TRUE(d.has_value());
  auto ga = lfs.getattr(*d);
  ASSERT_TRUE(ga.has_value()); EXPECT_TRUE(ga->is_dir);
}

TEST(LocalFs, StaleAfterUnlinkBeforeReuse) {
  Env e; FsLayer fs(e.im, e.dev); LocalFs lfs(fs, e.im);
  auto f = lfs.create(root(), "a.txt");
  ASSERT_TRUE(f.has_value());
  ASSERT_TRUE(lfs.unlink(root(), "a.txt").has_value());   // delete; the number is NOT reused
  auto w = lfs.write_file(*f, 0, "x");      // the generation is unchanged, so only an
  ASSERT_FALSE(w.has_value());              // allocation check can reject this write
  EXPECT_EQ(w.error(), Error::Stale);
}

TEST(LocalFs, StaleAfterInodeReuse) {
  Env e; FsLayer fs(e.im, e.dev); LocalFs lfs(fs, e.im);
  auto f = lfs.create(root(), "a.txt");     // get a handle (with the old generation)
  ASSERT_TRUE(f.has_value());
  FileHandle stale = *f;
  ASSERT_TRUE(lfs.unlink(root(), "a.txt").has_value());   // delete: frees the inode number
  auto g = lfs.create(root(), "b.txt");     // reuses the same inode number; generation +1
  ASSERT_TRUE(g.has_value());
  ASSERT_EQ(g->id, stale.id);               // confirms the same id really was reused
  auto r = lfs.read_file(stale, 0, 1);      // read with the old handle
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), Error::Stale);
}
