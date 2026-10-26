#include "nanofs/handle.h"

namespace nanofs {

Result<inode_id> resolve_handle(InodeManager& inodes, FileHandle h) {
  auto where = inodes.get(h.id);
  if (!where) {
    if (where.error() == Error::NotFound) return std::unexpected(Error::Stale);
    return std::unexpected(where.error());
  }
  auto gen = inodes.generation(h.id);
  if (!gen) return std::unexpected(gen.error());
  if (*gen != h.generation) return std::unexpected(Error::Stale);
  return h.id;
}

}  // namespace nanofs
