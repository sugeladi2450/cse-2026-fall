# librpc

This directory exposes the provided [rpclib](../third_party/rpclib/) library as
the CMake target `librpc`. It includes MessagePack serialization and Asio networking;
no additional system packages or Git submodules are needed.

Read the [nanoFS RPC protocol](../nanofs/proto/fs_rpc.md) and
[handlers](../nanofs/server/handlers.cc) for the filesystem messages.
`test/test_rpc_echo.cc` checks a client/server round trip independently of nanoFS.
The [Lab1B unit test script](../scripts/lab1/test-b.sh) includes this test.
