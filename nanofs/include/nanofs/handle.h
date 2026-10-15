#pragma once
#include <cstdint>
#include "nanofs/error.h"
#include "nanofs/inode.h"

namespace nanofs {

// NFS-style file handle: inode number + generation. The generation exposes a stale
// handle whose inode number was reused after a delete.
struct FileHandle {
  inode_id id;
  uint32_t generation;
};

// The root handle's generation is always 1: root is the first inode allocated by format(),
// so its generation goes 0 -> 1.
inline constexpr uint32_t ROOT_GENERATION = 1;

// Validate a handle: if the inode number is currently allocated (InodeManager::get) and
// h.generation equals its current generation (InodeManager::generation), return h.id.
// Unallocated or out-of-range inode number, or generation mismatch: Error::Stale.
// Other storage errors, such as IoError, are propagated.
Result<inode_id> resolve_handle(InodeManager& inodes, FileHandle h);

}  // namespace nanofs
