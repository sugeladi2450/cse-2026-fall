#pragma once
// rpclib's bundled msgpack header (v1/cpp_config_decl.hpp) uses
// std::remove_volatile etc. without #including <type_traits> itself. Under
// C++17 it happened to come in indirectly via <memory>/<tuple>; under C++23
// libc++ no longer provides it that way, so include it here explicitly, before
// rpc/server.h.
#include <type_traits>
#include <rpc/server.h>
#include "nanofs/fs.h"
#include "nanofs/inode.h"
namespace nanofs {
// Bind the ten fs_* methods on srv, backed by fs, using inodes to validate
// handles. On the wire a handle is (inode_id, gen): each handler first calls
// resolve_handle to check the generation (mismatch -> err 70/Stale), then
// forwards to fs. Every reply is an envelope [err:int, result]: err=0 means
// success, any other value is an errno code; on failure result is a default
// value (the client only looks at err).
void register_fs_handlers(rpc::server& srv, FsLayer& fs, InodeManager& inodes);
}
