#include <nanofs/handlers.h>
#include <nanofs/fs.h>
#include <nanofs/inode.h>
#include <nanofs/block.h>
#include <nanofs/handle.h>
#include <rpc/server.h>
#include <rpc/client.h>
#include <gtest/gtest.h>
#include <tuple>
#include <string>
#include <vector>
using namespace nanofs;

TEST(Handlers, RoundTripEachMethodOverRpc) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64); ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  rpc::server srv(18090);
  register_fs_handlers(srv, fs, im);
  srv.async_run(1);
  rpc::client c("127.0.0.1", 18090);
  const uint32_t rg = ROOT_GENERATION;

  // create -> [0, [inode_id, gen]]
  auto cr = c.call("fs_create", (uint32_t)ROOT_INODE, rg, std::string("a.txt"))
              .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  ASSERT_EQ(std::get<0>(cr), 0);
  auto [fid, fgen] = std::get<1>(cr);

  // write
  auto wr = c.call("fs_write", fid, fgen, (uint32_t)0, std::string("hello"))
              .as<std::tuple<int, uint32_t>>();
  EXPECT_EQ(std::get<0>(wr), 0); EXPECT_EQ(std::get<1>(wr), 5u);

  // read
  auto rd = c.call("fs_read", fid, fgen, (uint32_t)0, (uint32_t)5)
              .as<std::tuple<int, std::string>>();
  EXPECT_EQ(std::get<0>(rd), 0); EXPECT_EQ(std::get<1>(rd), "hello");

  // lookup
  auto lk = c.call("fs_lookup", (uint32_t)ROOT_INODE, rg, std::string("a.txt"))
              .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  EXPECT_EQ(std::get<0>(lk), 0); EXPECT_EQ(std::get<0>(std::get<1>(lk)), fid);

  // getattr
  auto ga = c.call("fs_getattr", fid, fgen).as<std::tuple<int, std::tuple<uint32_t,bool,uint32_t>>>();
  EXPECT_EQ(std::get<0>(ga), 0); EXPECT_EQ(std::get<2>(std::get<1>(ga)), 5u);

  // readdir
  auto rr = c.call("fs_readdir", (uint32_t)ROOT_INODE, rg)
              .as<std::tuple<int, std::vector<std::tuple<std::string,uint32_t>>>>();
  EXPECT_EQ(std::get<0>(rr), 0); EXPECT_EQ(std::get<1>(rr).size(), 1u);

  // mkdir
  auto mk = c.call("fs_mkdir", (uint32_t)ROOT_INODE, rg, std::string("d"))
              .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  EXPECT_EQ(std::get<0>(mk), 0);

  // setattr
  auto sa = c.call("fs_setattr", fid, fgen, (uint32_t)3)
              .as<std::tuple<int, std::tuple<uint32_t,bool,uint32_t>>>();
  EXPECT_EQ(std::get<0>(sa), 0); EXPECT_EQ(std::get<2>(std::get<1>(sa)), 3u);

  // unlink
  auto ul = c.call("fs_unlink", (uint32_t)ROOT_INODE, rg, std::string("a.txt")).as<std::tuple<int, bool>>();
  EXPECT_EQ(std::get<0>(ul), 0);

  // rmdir empty dir "d" -> ok
  auto rd_ok = c.call("fs_rmdir", (uint32_t)ROOT_INODE, rg, std::string("d")).as<std::tuple<int, bool>>();
  EXPECT_EQ(std::get<0>(rd_ok), 0);

  // error envelope: lookup missing -> [2, ...]
  auto miss = c.call("fs_lookup", (uint32_t)ROOT_INODE, rg, std::string("gone"))
                .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  EXPECT_EQ(std::get<0>(miss), 2);   // ENOENT

  // stale: use a mismatched generation -> [70, ...]
  auto stale = c.call("fs_getattr", fid, fgen + 100).as<std::tuple<int, std::tuple<uint32_t,bool,uint32_t>>>();
  EXPECT_EQ(std::get<0>(stale), 70);   // ESTALE
}

// Duplicate-delivery idempotence: with at-least-once delivery, a request the
// server has already applied but whose ack was lost gets retransmitted by the
// client, so the same mutating call runs twice on the server. Effectively-once
// requires the second run to have no extra observable effect and to return
// exactly the same result (generation included) — otherwise the first handle
// the client cached would go stale for no reason.
// This guards what the local idempotence tests in test_fs.cc cannot: the
// idempotence still holds after crossing the wire and the generation encoding.
TEST(Handlers, DuplicateDeliveryIsIdempotent) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64); ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  rpc::server srv(18093);
  register_fs_handlers(srv, fs, im);
  srv.async_run(1);
  rpc::client c("127.0.0.1", 18093);
  const uint32_t rg = ROOT_GENERATION;

  // create ×2: the second run takes the "already exists -> return the existing
  // handle" branch; it must return the same {id, gen} and add no directory
  // entry.
  auto cr1 = c.call("fs_create", (uint32_t)ROOT_INODE, rg, std::string("a.txt"))
               .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  auto cr2 = c.call("fs_create", (uint32_t)ROOT_INODE, rg, std::string("a.txt"))
               .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  ASSERT_EQ(std::get<0>(cr1), 0);
  ASSERT_EQ(std::get<0>(cr2), 0);
  EXPECT_EQ(std::get<1>(cr1), std::get<1>(cr2));   // same id, same gen (otherwise the first handle would go stale for no reason)
  auto [fid, fgen] = std::get<1>(cr1);
  auto rr = c.call("fs_readdir", (uint32_t)ROOT_INODE, rg)
              .as<std::tuple<int, std::vector<std::tuple<std::string,uint32_t>>>>();
  EXPECT_EQ(std::get<1>(rr).size(), 1u);           // no duplicate directory entry

  // write ×2 (absolute offset): replaying the same write leaves the content
  // unchanged.
  for (int i = 0; i < 2; ++i)
    EXPECT_EQ(std::get<0>(c.call("fs_write", fid, fgen, (uint32_t)0, std::string("hello"))
                            .as<std::tuple<int, uint32_t>>()), 0);
  auto rd = c.call("fs_read", fid, fgen, (uint32_t)0, (uint32_t)5).as<std::tuple<int, std::string>>();
  EXPECT_EQ(std::get<1>(rd), "hello");

  // unlink ×2: the second run (the retransmit) must still be ok, never ENOENT
  // — otherwise a retransmit would turn an already-successful delete into an
  // error.
  EXPECT_EQ(std::get<0>(c.call("fs_unlink", (uint32_t)ROOT_INODE, rg, std::string("a.txt"))
                          .as<std::tuple<int, bool>>()), 0);
  EXPECT_EQ(std::get<0>(c.call("fs_unlink", (uint32_t)ROOT_INODE, rg, std::string("a.txt"))
                          .as<std::tuple<int, bool>>()), 0);

  // mkdir ×2 + rmdir ×2: the same guarantee, on the directory side.
  auto mk1 = c.call("fs_mkdir", (uint32_t)ROOT_INODE, rg, std::string("d"))
               .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  auto mk2 = c.call("fs_mkdir", (uint32_t)ROOT_INODE, rg, std::string("d"))
               .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  ASSERT_EQ(std::get<0>(mk1), 0); ASSERT_EQ(std::get<0>(mk2), 0);
  EXPECT_EQ(std::get<1>(mk1), std::get<1>(mk2));
  EXPECT_EQ(std::get<0>(c.call("fs_rmdir", (uint32_t)ROOT_INODE, rg, std::string("d"))
                          .as<std::tuple<int, bool>>()), 0);
  EXPECT_EQ(std::get<0>(c.call("fs_rmdir", (uint32_t)ROOT_INODE, rg, std::string("d"))
                          .as<std::tuple<int, bool>>()), 0);
}

TEST(Handlers, RmdirNonEmptyReturnsNotEmpty) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64); ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  rpc::server srv(18091);
  register_fs_handlers(srv, fs, im);
  srv.async_run(1);
  rpc::client c("127.0.0.1", 18091);
  const uint32_t rg = ROOT_GENERATION;

  auto mk = c.call("fs_mkdir", (uint32_t)ROOT_INODE, rg, std::string("d"))
              .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  ASSERT_EQ(std::get<0>(mk), 0);
  auto [did, dgen] = std::get<1>(mk);
  // Create a child under d so it is not empty.
  ASSERT_EQ(std::get<0>(c.call("fs_create", did, dgen, std::string("f"))
                          .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>()), 0);
  auto rd = c.call("fs_rmdir", (uint32_t)ROOT_INODE, rg, std::string("d")).as<std::tuple<int, bool>>();
  EXPECT_EQ(std::get<0>(rd), 39);   // ENOTEMPTY
}

TEST(Handlers, EveryMethodRejectsStaleHandleBeforeForwarding) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64); ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  rpc::server srv(18092);
  register_fs_handlers(srv, fs, im);
  srv.async_run(1);
  rpc::client c("127.0.0.1", 18092);
  const uint32_t bad_root_gen = ROOT_GENERATION + 100;

  auto cr = c.call("fs_create", (uint32_t)ROOT_INODE, ROOT_GENERATION, std::string("a.txt"))
              .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  ASSERT_EQ(std::get<0>(cr), 0);
  auto [fid, fgen] = std::get<1>(cr);
  auto mk = c.call("fs_mkdir", (uint32_t)ROOT_INODE, ROOT_GENERATION, std::string("d"))
              .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  ASSERT_EQ(std::get<0>(mk), 0);

  auto expect_stale = [](const char* name, int err) {
    EXPECT_EQ(err, 70) << name;
  };

  expect_stale("fs_lookup",
      std::get<0>(c.call("fs_lookup", (uint32_t)ROOT_INODE, bad_root_gen, std::string("a.txt"))
                    .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>()));
  expect_stale("fs_getattr",
      std::get<0>(c.call("fs_getattr", fid, fgen + 100)
                    .as<std::tuple<int, std::tuple<uint32_t,bool,uint32_t>>>()));
  expect_stale("fs_readdir",
      std::get<0>(c.call("fs_readdir", (uint32_t)ROOT_INODE, bad_root_gen)
                    .as<std::tuple<int, std::vector<std::tuple<std::string,uint32_t>>>>()));
  expect_stale("fs_read",
      std::get<0>(c.call("fs_read", fid, fgen + 100, (uint32_t)0, (uint32_t)1)
                    .as<std::tuple<int, std::string>>()));
  expect_stale("fs_write",
      std::get<0>(c.call("fs_write", fid, fgen + 100, (uint32_t)0, std::string("x"))
                    .as<std::tuple<int, uint32_t>>()));
  expect_stale("fs_create",
      std::get<0>(c.call("fs_create", (uint32_t)ROOT_INODE, bad_root_gen, std::string("b.txt"))
                    .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>()));
  expect_stale("fs_mkdir",
      std::get<0>(c.call("fs_mkdir", (uint32_t)ROOT_INODE, bad_root_gen, std::string("e"))
                    .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>()));
  expect_stale("fs_unlink",
      std::get<0>(c.call("fs_unlink", (uint32_t)ROOT_INODE, bad_root_gen, std::string("a.txt"))
                    .as<std::tuple<int, bool>>()));
  expect_stale("fs_rmdir",
      std::get<0>(c.call("fs_rmdir", (uint32_t)ROOT_INODE, bad_root_gen, std::string("d"))
                    .as<std::tuple<int, bool>>()));
  expect_stale("fs_setattr",
      std::get<0>(c.call("fs_setattr", fid, fgen + 100, (uint32_t)0)
                    .as<std::tuple<int, std::tuple<uint32_t,bool,uint32_t>>>()));
}
