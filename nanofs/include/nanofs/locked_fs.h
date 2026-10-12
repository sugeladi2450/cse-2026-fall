#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nanofs/allocator_lock.h"
#include "nanofs/inode_lock_table.h"
#include "nanofs/txfs.h"

namespace nanofs {

// Locking fs layer giving before-or-after atomicity (isolation / serializability)
// for concurrent access. It sits on TxFs (which gives each write all-or-nothing
// via the journal):
//   - Reads take a per-inode *shared* lock and delegate to FsLayer (no
//     transaction); readers of the same inode run concurrently, and reads of one
//     inode never block a write to another.
//   - Writes take a per-inode *exclusive* lock plus an AllocatorLock scope and
//     delegate to TxFs (journaled). The locks are held for the whole op,
//     including the commit inside TxFs — strict two-phase locking. The allocator lock is a
//     lazy lock: it only engages if the op actually allocates/frees, serializing
//     that to prevent the whole-block-redo clobber on shared bitmap/table blocks.
//
// Deadlock-free: the only op needing two inode locks (unlink/rmdir) takes them
// parent-then-child; nanofs has no hardlink or rename, so each inode has at most
// one parent and writers on distinct parents share no locks. The lock order is
// inode (parent before child) -> allocator lock -> journal lock.
class LockedFs : public TxFs {
 public:
  LockedFs(InodeManager& inodes, JournaledBlockDevice& jdev, std::uint32_t num_inodes)
      : TxFs(inodes, jdev), locks_(num_inodes + 1), num_inodes_(num_inodes) {}

  Result<inode_id> lookup(inode_id parent, const std::string& name) override;
  Result<Attr> getattr(inode_id id) override;
  Result<std::vector<DirEntry>> readdir(inode_id dir) override;
  Result<std::string> read_file(inode_id id, uint32_t off, uint32_t len) override;

  Result<uint32_t> write_file(inode_id id, uint32_t off, const std::string& data) override;
  Result<void> unlink(inode_id parent, const std::string& name) override;
  Result<void> rmdir(inode_id parent, const std::string& name) override;
  Result<Attr> setattr(inode_id id, uint32_t new_size) override;

 protected:
  Result<inode_id> make_node(inode_id parent, const std::string& name, uint32_t type) override;

 private:
  // A valid (lockable) inode id: non-nil and within the table. Out-of-range ids
  // (possible from an untrusted RPC client) bypass locking and delegate to the
  // base, which reports the error instead of indexing the table out of bounds.
  bool lockable(inode_id id) const { return id != 0 && id <= num_inodes_; }

  InodeLockTable locks_;
  AllocatorLock alloc_lock_;
  std::uint32_t num_inodes_;
};

}  // namespace nanofs
