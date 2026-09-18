#pragma once
#include <expected>
#include <string_view>

namespace nanofs {
// Error type. The names avoid the errno macros on purpose (EIO/EINVAL/... are macros from
// <cerrno> and would clash with enum tokens). errno equivalents: NotFound=ENOENT,
// NoSpace=ENOSPC, Exists=EEXIST, NotDir=ENOTDIR, IsDir=EISDIR, Invalid=EINVAL, IoError=EIO,
// NotEmpty=ENOTEMPTY, Stale=ESTALE. The numeric mapping happens at the RPC boundary
// (proto/fs_rpc.md).
enum class Error { NotFound, NoSpace, Exists, NotDir, IsDir, Invalid, IoError, NotEmpty, Stale };

// Rust-Result style: either a success value or an Error, never both.
template <class T>
using Result = std::expected<T, Error>;

// Human-readable error name for logs. Not the errno mapping; that lives in
// handlers.cc/err_code and fuse-adaptor/main.cc/to_errno, each with its own numbering.
inline std::string_view to_string(Error e) {
  switch (e) {
    case Error::NotFound: return "NotFound";
    case Error::NoSpace:  return "NoSpace";
    case Error::Exists:   return "Exists";
    case Error::NotDir:   return "NotDir";
    case Error::IsDir:    return "IsDir";
    case Error::Invalid:  return "Invalid";
    case Error::IoError:  return "IoError";
    case Error::NotEmpty: return "NotEmpty";
    case Error::Stale:    return "Stale";
  }
  return "Unknown";
}
}  // namespace nanofs
