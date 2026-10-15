// test_remote_fs.cc · §4 client fault-tolerance unit tests
//
// Suites are grouped by function. Students implement call_with_retry; RemoteFs is a
// provided forwarding layer, and its end-to-end behavior (including stale
// handles and a dead server) is guarded by two integration tests.
//
//   function                         │ test (suite.case)
//   ─────────────────────────────────┼──────────────────────────────────────────
//   call_with_retry                  │ CallWithRetry.RetriesTransientElseGivesUpIoError
//                                    │ CallWithRetry.DoesNotRetryProtocolErrors
//                                    │ CallWithRetry.LostAckRetransmitReappliesIdempotently
//   RemoteFs (provided, integration) │ RemoteFs.RoundTripMatchesLocalAndStale
//                                    │ RemoteFs.DeadServerGivesIoError
#include <nanofs/remote_fs.h>
#include <nanofs/handlers.h>
#include <nanofs/fs.h>
#include <nanofs/inode.h>
#include <nanofs/block.h>
#include <rpc/server.h>
#include <gtest/gtest.h>
#include <stdexcept>
using namespace nanofs;

TEST(CallWithRetry, RetriesTransientElseGivesUpIoError) {
  // The first two attempts throw, the third succeeds => retry until success,
  // 3 calls in total.
  int calls = 0;
  auto ok = call_with_retry([&]() -> Result<int> {
    if (++calls <= 2) throw std::runtime_error("transient");
    return 42;
  }, /*retries=*/3, /*backoff_ms=*/0);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(*ok, 42);
  EXPECT_EQ(calls, 3);

  // Every attempt throws => once the retries run out it returns IoError,
  // 1 + retries calls in total.
  int fails = 0;
  auto bad = call_with_retry([&]() -> Result<int> {
    ++fails;
    throw std::runtime_error("down");
  }, /*retries=*/3, /*backoff_ms=*/0);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), Error::IoError);
  EXPECT_EQ(fails, 4);            // 1 call + 3 retries
}

TEST(CallWithRetry, DoesNotRetryProtocolErrors) {
  int calls = 0;
  auto bad = call_with_retry([&]() -> Result<int> {
    ++calls;
    return std::unexpected(Error::Stale);
  }, /*retries=*/3, /*backoff_ms=*/0);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), Error::Stale);
  EXPECT_EQ(calls, 1);
}

TEST(CallWithRetry, LostAckRetransmitReappliesIdempotently) {
  // Unlike Handlers.DuplicateDeliveryIsIdempotent, which calls the handler twice
  // by hand, here the retransmit is call_with_retry's own doing: the first attempt
  // is applied on the server, its ack is dropped, and the retry re-applies it.
  // Checks duplicate create delivery without intervening conflicting operations.
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64); ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  rpc::server srv(18097);
  register_fs_handlers(srv, fs, im);
  srv.async_run(1);
  rpc::client c("127.0.0.1", 18097);
  const uint32_t rg = ROOT_GENERATION;

  int deliveries = 0;
  auto res = call_with_retry([&]() -> Result<std::tuple<uint32_t,uint32_t>> {
    auto [err, h] = c.call("fs_create", (uint32_t)ROOT_INODE, rg, std::string("a.txt"))
                      .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
    // The server has already executed the call; simulate the ack getting lost
    // on the way back — discard the result we already hold and throw, forcing
    // the client to retransmit.
    if (++deliveries == 1) throw std::runtime_error("ack lost on return path");
    if (err != 0) return std::unexpected(code_to_error(err));
    return h;
  }, /*retries=*/3, /*backoff_ms=*/0);

  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(deliveries, 2);   // one lost ack -> exactly one retransmit

  // effectively-once: the server ran create twice, but there is only one
  // directory entry.
  auto rr = c.call("fs_readdir", (uint32_t)ROOT_INODE, rg)
              .as<std::tuple<int, std::vector<std::tuple<std::string,uint32_t>>>>();
  EXPECT_EQ(std::get<1>(rr).size(), 1u);

  // The handle the client ends up with matches the one that really exists on
  // the server (the generation did not drift because of the retransmit -> no
  // surprise stale errors).
  auto lk = c.call("fs_lookup", (uint32_t)ROOT_INODE, rg, std::string("a.txt"))
              .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>();
  EXPECT_EQ(std::get<1>(lk), *res);
}

TEST(RemoteFs, RoundTripMatchesLocalAndStale) {
  SimulatedBlockDevice dev(4096, 256);
  InodeManager im(dev, 64); ASSERT_TRUE(im.format().has_value());
  FsLayer fs(im, dev);
  rpc::server srv(18095);
  register_fs_handlers(srv, fs, im);
  srv.async_run(1);

  RemoteFs r("127.0.0.1", 18095);
  FileHandle root{ROOT_INODE, ROOT_GENERATION};

  auto f = r.create(root, "a.txt");
  ASSERT_TRUE(f.has_value());
  ASSERT_TRUE(r.write_file(*f, 0, "hello").has_value());
  auto rd = r.read_file(*f, 0, 5);
  ASSERT_TRUE(rd.has_value()); EXPECT_EQ(*rd, "hello");
  auto dir = r.readdir(root);
  ASSERT_TRUE(dir.has_value()); EXPECT_EQ(dir->size(), 1u);

  FileHandle stale = *f;
  ASSERT_TRUE(r.unlink(root, "a.txt").has_value());
  auto g = r.create(root, "b.txt");     // reuses the inode number; the generation changes
  ASSERT_TRUE(g.has_value()); ASSERT_EQ(g->id, stale.id);
  auto bad = r.read_file(stale, 0, 1);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), Error::Stale);
}

TEST(RemoteFs, DeadServerGivesIoError) {
  RemoteFs r("127.0.0.1", 18096, /*retries=*/1, /*backoff_ms=*/0, /*timeout_ms=*/200);
  auto g = r.getattr(FileHandle{ROOT_INODE, ROOT_GENERATION});
  ASSERT_FALSE(g.has_value());
  EXPECT_EQ(g.error(), Error::IoError);
}
