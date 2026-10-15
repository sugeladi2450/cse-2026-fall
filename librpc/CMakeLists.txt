# 干净的再导出名：nanofs 其余部分只链接 `librpc`，不直接依赖 rpclib 的原始目标 `rpc`。
# vendored rpclib 的目标名是 `rpc`（无 rpclib:: 命名空间别名）。
add_library(librpc INTERFACE)
target_link_libraries(librpc INTERFACE rpc)

# echo 冒烟测试：证明 RPC 通道通。
add_executable(test_rpc_echo test/test_rpc_echo.cc)
set_target_properties(test_rpc_echo PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(test_rpc_echo PRIVATE librpc GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(test_rpc_echo)
