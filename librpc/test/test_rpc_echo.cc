#include <rpc/server.h>
#include <rpc/client.h>
#include <gtest/gtest.h>
#include <string>

// 冒烟测试：RPC 通道能把一个 payload 经 bind/call 原样打回来。
// 这是 P0 的验收——证明「先有一条被证明能通的 RPC 通道」，再谈任何 fs 逻辑。
TEST(RpcEcho, RoundTripsPayload) {
  rpc::server srv(18080);
  srv.bind("echo", [](std::string s) { return s; });
  srv.async_run(1);
  rpc::client c("127.0.0.1", 18080);
  EXPECT_EQ(c.call("echo", std::string("ping")).as<std::string>(), "ping");
}
