#pragma once
#include "nanofs/fs.h"
#include "nanofs/journal.h"

namespace nanofs {

// Transaction decorator: wraps every mutating FsLayer op in one journal transaction
// (begin -> op -> commit on success / abort on failure), so a single fs op is
// all-or-nothing across a crash. Reads are inherited from FsLayer unwrapped.
// Splitting file content from metadata is the block layer's job (BlockKind), not
// this class's; it only draws transaction boundaries.
class TxFs : public FsLayer {
 public:
  // jdev is both the block device handed to FsLayer and the transaction controller.
  TxFs(InodeManager& inodes, JournaledBlockDevice& jdev)
      : FsLayer(inodes, jdev), jdev_(jdev) {}

  Result<uint32_t> write_file(inode_id id, uint32_t off, const std::string& data) override;
  Result<void> unlink(inode_id parent, const std::string& name) override;
  Result<void> rmdir(inode_id parent, const std::string& name) override;
  Result<Attr> setattr(inode_id id, uint32_t new_size) override;

 protected:
  // Overriding this wraps both create and mkdir (both go through make_node).
  Result<inode_id> make_node(inode_id parent, const std::string& name, uint32_t type) override;

 private:
  JournaledBlockDevice& jdev_;
};

}  // namespace nanofs
