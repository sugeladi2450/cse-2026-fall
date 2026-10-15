#include "nanofs/handle.h"

namespace nanofs {

Result<inode_id> resolve_handle(InodeManager& inodes, FileHandle h) {
  /* TODO Lab1B: Your code here */
  return std::unexpected(Error::IoError);
}

}  // namespace nanofs
