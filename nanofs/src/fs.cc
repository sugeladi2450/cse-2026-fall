// FS layer implementation (spec: include/nanofs/fs.h)
#include "nanofs/fs.h"
#include <cstring>
#include <algorithm>
#include <limits>

namespace nanofs {

Result<Superblock> FsLayer::super() {
  std::vector<std::byte> buf(dev_.block_size());
  if (auto r = dev_.read_block(0, buf); !r) return std::unexpected(r.error());
  Superblock sb{};
  std::memcpy(&sb, buf.data(), sizeof(sb));
  return sb;
}

// Read one block as an array of block_id (an indirect block).
static Result<std::vector<block_id>> read_ptrs(BlockDevice& dev, block_id blk) {
  std::vector<std::byte> buf(dev.block_size());
  if (auto r = dev.read_block(blk, buf); !r) return std::unexpected(r.error());
  std::vector<block_id> ptrs(dev.block_size() / sizeof(block_id));
  std::memcpy(ptrs.data(), buf.data(), ptrs.size() * sizeof(block_id));
  return ptrs;
}
static Result<void> write_ptrs(BlockDevice& dev, block_id blk, const std::vector<block_id>& ptrs) {
  std::vector<std::byte> buf(dev.block_size(), std::byte{0});
  std::memcpy(buf.data(), ptrs.data(), std::min(buf.size(), ptrs.size() * sizeof(block_id)));
  return dev.write_block(blk, buf);
}

Result<block_id> FsLayer::logical_to_physical_block(inode& node, uint32_t idx,
                                                    bool allocate,
                                                    BlockAllocator& balloc,
                                                    uint32_t bs) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

Result<std::string> FsLayer::read_bytes(inode& node, uint32_t off,
                                        uint32_t len) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

// TODO Lab2A: classify writes here as BlockKind::Data for regular-file
// contents and BlockKind::Meta for directory contents.
Result<uint32_t> FsLayer::write_bytes(inode_id id, inode& node, uint32_t off,
                                      const std::string& data) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

Result<Attr> FsLayer::getattr(inode_id id) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

Result<std::string> FsLayer::read_file(inode_id id, uint32_t off, uint32_t len) {
  if (off + static_cast<uint64_t>(len) > std::numeric_limits<uint32_t>::max())
    return std::unexpected(Error::Invalid);
  auto node = inodes_.read_inode(id);
  if (!node) return std::unexpected(node.error());
  if (node->type == INODE_DIR) return std::unexpected(Error::IsDir);
  return read_bytes(*node, off, len);
}

Result<uint32_t> FsLayer::write_file(inode_id id, uint32_t off,
                                     const std::string& data) {
  if (data.size() > std::numeric_limits<uint32_t>::max() ||
      off + static_cast<uint64_t>(data.size()) >
          std::numeric_limits<uint32_t>::max())
    return std::unexpected(Error::Invalid);
  auto node = inodes_.read_inode(id);
  if (!node) return std::unexpected(node.error());
  if (node->type == INODE_DIR) return std::unexpected(Error::IsDir);
  return write_bytes(id, *node, off, data);
}

// The directory layout (fs.h) is deliberately simple so the three helpers stay short.
// The cost: short names waste the rest of the slot, lookup scans linearly past holes,
// and the file is never compacted. You may go further (variable-length records,
// compaction on delete, a hash index); the tests do not require it.
Result<std::vector<DirEntry>> FsLayer::read_dir_entries(inode_id dir) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

Result<inode_id> FsLayer::lookup(inode_id parent, const std::string& name) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

// Provided: public entry for read_dir_entries.
Result<std::vector<DirEntry>> FsLayer::readdir(inode_id dir) {
  return read_dir_entries(dir);
}

Result<void> FsLayer::add_dir_entry(inode_id dir, const std::string& name,
                                    inode_id target) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

Result<void> FsLayer::remove_dir_entry(inode_id dir, const std::string& name) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

// TODO Lab2B: keep this operation inside its existing parent lock; avoid
// re-entering LockedFs through a virtual lookup.
Result<inode_id> FsLayer::make_node(inode_id parent, const std::string& name,
                                    uint32_t type) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

// Provided: create/mkdir differ only in the inode type; the work is in make_node.
Result<inode_id> FsLayer::create(inode_id parent, const std::string& name) {
  return make_node(parent, name, INODE_FILE);
}

Result<inode_id> FsLayer::mkdir(inode_id parent, const std::string& name) {
  return make_node(parent, name, INODE_DIR);
}

Result<void> FsLayer::free_file_blocks(inode& node, BlockAllocator& balloc,
                                       uint32_t bs) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

// TODO Lab2B: avoid re-entering LockedFs for this internal lookup.
Result<void> FsLayer::unlink(inode_id parent, const std::string& name) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

// TODO Lab2B: avoid re-entering LockedFs for this internal lookup.
Result<void> FsLayer::rmdir(inode_id parent, const std::string& name) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}

Result<Attr> FsLayer::setattr(inode_id id, uint32_t new_size) {
  /* TODO Lab1A: Your code here */
  return std::unexpected(Error::IoError);
}
}  // namespace nanofs
