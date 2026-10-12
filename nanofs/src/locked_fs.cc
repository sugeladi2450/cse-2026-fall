#include "nanofs/locked_fs.h"

#include <shared_mutex>

namespace nanofs {

Result<inode_id> LockedFs::lookup(inode_id parent, const std::string& name) {
  /* TODO Lab2B: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<Attr> LockedFs::getattr(inode_id id) {
  /* TODO Lab2B: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<std::vector<DirEntry>> LockedFs::readdir(inode_id dir) {
  /* TODO Lab2B: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<std::string> LockedFs::read_file(inode_id id, uint32_t off,
                                        uint32_t len) {
  /* TODO Lab2B: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<uint32_t> LockedFs::write_file(inode_id id, uint32_t off,
                                      const std::string& data) {
  /* TODO Lab2B: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<Attr> LockedFs::setattr(inode_id id, uint32_t new_size) {
  /* TODO Lab2B: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<inode_id> LockedFs::make_node(inode_id parent, const std::string& name,
                                     uint32_t type) {
  /* TODO Lab2B: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<void> LockedFs::unlink(inode_id parent, const std::string& name) {
  /* TODO Lab2B: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<void> LockedFs::rmdir(inode_id parent, const std::string& name) {
  /* TODO Lab2B: Your code here */
  return std::unexpected(Error::Invalid);
}

}  // namespace nanofs
