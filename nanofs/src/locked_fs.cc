#include "nanofs/locked_fs.h"

#include <shared_mutex>

namespace nanofs {

Result<inode_id> LockedFs::lookup(inode_id parent, const std::string& name) {
  if (!lockable(parent)) return TxFs::lookup(parent, name);
  auto g = locks_.lock_shared(parent);
  return TxFs::lookup(parent, name);
}

Result<Attr> LockedFs::getattr(inode_id id) {
  if (!lockable(id)) return TxFs::getattr(id);
  auto g = locks_.lock_shared(id);
  return TxFs::getattr(id);
}

Result<std::vector<DirEntry>> LockedFs::readdir(inode_id dir) {
  if (!lockable(dir)) return TxFs::readdir(dir);
  auto g = locks_.lock_shared(dir);
  return TxFs::readdir(dir);
}

Result<std::string> LockedFs::read_file(inode_id id, uint32_t off,
                                        uint32_t len) {
  if (!lockable(id)) return TxFs::read_file(id, off, len);
  auto g = locks_.lock_shared(id);
  return TxFs::read_file(id, off, len);
}

Result<uint32_t> LockedFs::write_file(inode_id id, uint32_t off,
                                      const std::string& data) {
  if (!lockable(id)) return TxFs::write_file(id, off, data);
  auto g = locks_.lock_exclusive(id);
  AllocatorLock::Scope scope(alloc_lock_);
  return TxFs::write_file(id, off, data);
}

Result<Attr> LockedFs::setattr(inode_id id, uint32_t new_size) {
  if (!lockable(id)) return TxFs::setattr(id, new_size);
  auto g = locks_.lock_exclusive(id);
  AllocatorLock::Scope scope(alloc_lock_);
  return TxFs::setattr(id, new_size);
}

Result<inode_id> LockedFs::make_node(inode_id parent, const std::string& name,
                                     uint32_t type) {
  if (!lockable(parent)) return TxFs::make_node(parent, name, type);
  auto g = locks_.lock_exclusive(parent);
  AllocatorLock::Scope scope(alloc_lock_);
  return TxFs::make_node(parent, name, type);
}

Result<void> LockedFs::unlink(inode_id parent, const std::string& name) {
  if (!lockable(parent)) return TxFs::unlink(parent, name);
  auto pg = locks_.lock_exclusive(parent);
  std::unique_lock<std::shared_mutex> cg;
  if (auto child = FsLayer::lookup(parent, name); child && lockable(*child))
    cg = locks_.lock_exclusive(*child);
  AllocatorLock::Scope scope(alloc_lock_);
  return TxFs::unlink(parent, name);
}

Result<void> LockedFs::rmdir(inode_id parent, const std::string& name) {
  if (!lockable(parent)) return TxFs::rmdir(parent, name);
  auto pg = locks_.lock_exclusive(parent);
  std::unique_lock<std::shared_mutex> cg;
  if (auto child = FsLayer::lookup(parent, name); child && lockable(*child))
    cg = locks_.lock_exclusive(*child);
  AllocatorLock::Scope scope(alloc_lock_);
  return TxFs::rmdir(parent, name);
}

}  // namespace nanofs
