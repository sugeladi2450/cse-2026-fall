#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <type_traits>
#include "nanofs/error.h"
#include "nanofs/block.h"
#include "nanofs/inode.h"

namespace nanofs {

// Metadata returned by getattr.
struct Attr { uint32_t inode_id; bool is_dir; uint32_t size; };

inline constexpr uint32_t NANOFS_NAME_MAX = 60;
// Directory entry: fixed 64 bytes. inode_id == 0 marks a free slot; name is NUL-terminated (<= 59 chars).
struct DirEntry { uint32_t inode_id; char name[NANOFS_NAME_MAX]; };
static_assert(sizeof(DirEntry) == 64, "DirEntry must be 64 bytes");
static_assert(std::is_trivially_copyable_v<DirEntry>);

// File and directory operations on top of the provided inode/block layers; the
// backend the RPC handlers call into. Public methods map one-to-one onto the ten
// fs_* RPCs; everything returns Result<>.
class FsLayer {
 public:
  FsLayer(InodeManager& inodes, BlockDevice& dev) : inodes_(inodes), dev_(dev) {}
  virtual ~FsLayer() = default;

  virtual Result<inode_id> lookup(inode_id parent, const std::string& name);
  virtual Result<Attr> getattr(inode_id id);
  virtual Result<std::vector<DirEntry>> readdir(inode_id dir);
  virtual Result<std::string> read_file(inode_id id, uint32_t off, uint32_t len);
  virtual Result<uint32_t> write_file(inode_id id, uint32_t off, const std::string& data);
  Result<inode_id> create(inode_id parent, const std::string& name);
  Result<inode_id> mkdir(inode_id parent, const std::string& name);
  virtual Result<void> unlink(inode_id parent, const std::string& name);
  // Remove the empty directory `name`. Not a directory -> NotDir; not empty -> NotEmpty;
  // missing -> ok (idempotent, like unlink).
  virtual Result<void> rmdir(inode_id parent, const std::string& name);
  virtual Result<Attr> setattr(inode_id id, uint32_t new_size);

 private:
  Result<Superblock> super();
  // Map the file's logical_index-th block to a disk block, walking direct -> single
  // indirect -> double indirect. With allocate=true, allocate data/indirect blocks along
  // the way and write them back (updates node.blocks[] and the on-disk indirect blocks).
  // Returns NULL_BLOCK (0) when allocate=false and the block is not allocated (a hole).
  Result<block_id> logical_to_physical_block(inode& node, uint32_t logical_index,
                                             bool allocate, BlockAllocator& balloc, uint32_t bs);
  // Read [off, off+len) of node's data, truncated at size; holes read as zero.
  // No type check: files and directories share it.
  Result<std::string> read_bytes(inode& node, uint32_t off, uint32_t len);
  // Write data at [off, ...) of node's data, growing as needed; update size and write
  // the inode back. Returns the number of bytes written.
  Result<uint32_t> write_bytes(inode_id id, inode& node, uint32_t off, const std::string& data);
  // Free all of node's content blocks (indirect blocks included), zero blocks[] and set
  // size = 0. Does not write the inode back; the caller does.
  Result<void> free_file_blocks(inode& node, BlockAllocator& balloc, uint32_t bs);
  // Directory helpers: a directory is a file holding a fixed-size DirEntry array.
  // read_dir_entries parses it, skipping free slots; add_dir_entry reuses a free slot
  // before growing; remove_dir_entry zeroes the slot (missing name -> ok).
  Result<std::vector<DirEntry>> read_dir_entries(inode_id dir);
  Result<void> add_dir_entry(inode_id dir, const std::string& name, inode_id target);
  Result<void> remove_dir_entry(inode_id dir, const std::string& name);

 protected:
  // Shared body of create/mkdir: parent must be a directory; an existing name returns
  // its inode; otherwise allocate an inode and link it (free it again if linking fails).
  virtual Result<inode_id> make_node(inode_id parent, const std::string& name, uint32_t type);

 private:
  InodeManager& inodes_;
  BlockDevice& dev_;
};
}  // namespace nanofs
