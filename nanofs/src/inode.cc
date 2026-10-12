// Inode layer implementation (spec: include/nanofs/inode.h)
//
// Implements format(), allocate_inode/free_inode plus the helpers they rely on
// (read_super/get/set_table), and read_inode/write_inode.

#include "nanofs/inode.h"
#include "nanofs/allocator_lock.h"
#include <cstring>
#include <vector>

namespace nanofs {
namespace {
// ceil(a/b)
uint32_t ceil_div(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
}  // namespace

InodeManager::InodeManager(BlockDevice& dev, uint32_t num_inodes)
    : dev_(dev), num_inodes_(num_inodes) {}

Result<Superblock> InodeManager::read_super() {
  std::vector<std::byte> buf(dev_.block_size());
  if (auto r = dev_.read_block(0, buf); !r) return std::unexpected(r.error());
  Superblock sb{};
  std::memcpy(&sb, buf.data(), sizeof(sb));
  return sb;
}

Result<InodeTableEntry> InodeManager::read_entry(inode_id id) {
  if (id == 0 || id > num_inodes_) return std::unexpected(Error::NotFound);
  auto sb = read_super();
  if (!sb) return std::unexpected(sb.error());
  const uint32_t bs = sb->block_size;
  const size_t byte_off = static_cast<size_t>(id) * sizeof(InodeTableEntry);
  const block_id blk = sb->inode_table_start + static_cast<block_id>(byte_off / bs);
  const size_t in_blk = byte_off % bs;
  std::vector<std::byte> buf(bs);
  if (auto r = dev_.read_block(blk, buf); !r) return std::unexpected(r.error());
  InodeTableEntry e{};
  std::memcpy(&e, buf.data() + in_blk, sizeof(e));
  return e;
}

Result<block_id> InodeManager::get(inode_id id) {
  auto e = read_entry(id);
  if (!e) return std::unexpected(e.error());
  if (e->where == 0) return std::unexpected(Error::NotFound);
  return e->where;
}

Result<uint32_t> InodeManager::generation(inode_id id) {
  auto e = read_entry(id);
  if (!e) return std::unexpected(e.error());
  return e->generation;
}

Result<void> InodeManager::set_table(inode_id id, block_id where, uint32_t generation) {
  auto sb = read_super();
  if (!sb) return std::unexpected(sb.error());
  const uint32_t bs = sb->block_size;
  const size_t byte_off = static_cast<size_t>(id) * sizeof(InodeTableEntry);
  const block_id blk = sb->inode_table_start + static_cast<block_id>(byte_off / bs);
  const size_t in_blk = byte_off % bs;
  std::vector<std::byte> buf(bs);
  if (auto r = dev_.read_block(blk, buf); !r) return r;
  InodeTableEntry e{where, generation};
  std::memcpy(buf.data() + in_blk, &e, sizeof(e));
  return dev_.write_block(blk, buf);
}

Result<inode> InodeManager::read_inode(inode_id id) {
  auto where = get(id);
  if (!where) return std::unexpected(where.error());
  std::vector<std::byte> buf(dev_.block_size());
  if (auto r = dev_.read_block(*where, buf); !r) return std::unexpected(r.error());
  inode node{};
  std::memcpy(&node, buf.data(), sizeof(node));
  return node;
}

Result<void> InodeManager::write_inode(inode_id id, const inode& node) {
  auto where = get(id);
  if (!where) return std::unexpected(where.error());
  std::vector<std::byte> buf(dev_.block_size());
  std::memcpy(buf.data(), &node, sizeof(node));
  return dev_.write_block(*where, buf);
}

Result<inode_id> InodeManager::allocate_inode(uint32_t type) {
  AllocatorLock::engage_current();
  auto sb = read_super();
  if (!sb) return std::unexpected(sb.error());
  BlockAllocator ialloc(dev_, sb->inode_bitmap_start, sb->inode_bitmap_blocks,
                        ROOT_INODE, num_inodes_ + 1);
  auto id = ialloc.allocate();
  if (!id) return std::unexpected(id.error());   // full -> NoSpace
  BlockAllocator balloc(dev_, sb->block_bitmap_start, sb->block_bitmap_blocks,
                        sb->data_start, sb->num_blocks);
  auto blk = balloc.allocate();
  if (!blk) { (void)ialloc.free(*id); return std::unexpected(blk.error()); }
  uint32_t gen = 1;  // Lab1A uses a fixed generation; Lab1B updates it on allocation.
  /* TODO Lab1B: Your code here */
  if (auto r = set_table(*id, *blk, gen); !r) {
    (void)balloc.free(*blk);
    (void)ialloc.free(*id);
    return std::unexpected(r.error());
  }
  inode node{};
  node.type = type;
  node.nlink = 1;
  if (auto r = write_inode(*id, node); !r) {
    (void)balloc.free(*blk);
    (void)ialloc.free(*id);
    return std::unexpected(r.error());
  }
  return *id;
}

Result<void> InodeManager::free_inode(inode_id id) {
  AllocatorLock::engage_current();
  auto e = read_entry(id);
  if (!e) return std::unexpected(e.error());
  if (e->where == 0) return std::unexpected(Error::NotFound);   // not allocated
  auto sb = read_super();
  if (!sb) return std::unexpected(sb.error());
  BlockAllocator balloc(dev_, sb->block_bitmap_start, sb->block_bitmap_blocks,
                        sb->data_start, sb->num_blocks);
  // Free the block holding the inode struct (allocated by InodeManager itself, not a content
  // block from inode.blocks[]), so an alloc/free cycle leaks nothing.
  if (auto r = balloc.free(e->where); !r) return r;
  // Clear where (keep generation) so get/read_inode/write_inode on this id fail with NotFound
  // until the id is allocated again. Otherwise a caller holding the old id (a stale handle,
  // the remote-handle task) would keep writing into a block that is no longer this inode's.
  if (auto r = set_table(id, 0, e->generation); !r) return r;
  BlockAllocator ialloc(dev_, sb->inode_bitmap_start, sb->inode_bitmap_blocks,
                        ROOT_INODE, num_inodes_ + 1);
  // Not atomic: if this step fails, the block is already freed and the table entry already
  // cleared, but the inode-bitmap bit stays set, so the inode number leaks.
  // Atomic recovery after storage errors is outside Lab1.
  return ialloc.free(id);   // does not free the file's content blocks inode.blocks[] (the fs layer does that)
}

Result<void> InodeManager::format() {
  const uint32_t bs = static_cast<uint32_t>(dev_.block_size());
  const uint32_t nb = static_cast<uint32_t>(dev_.num_blocks());
  if (bs < sizeof(inode)) return std::unexpected(Error::Invalid);

  // Layout: [0]=super, inode table, inode bitmap, block bitmap, data
  Superblock sb{};
  sb.magic = NANOFS_MAGIC;
  sb.block_size = bs;
  sb.num_blocks = nb;
  sb.num_inodes = num_inodes_;
  uint32_t cur = 1;
  sb.inode_table_start = cur;
  sb.inode_table_blocks = ceil_div((num_inodes_ + 1) * sizeof(InodeTableEntry), bs);
  cur += sb.inode_table_blocks;
  sb.inode_bitmap_start = cur;
  sb.inode_bitmap_blocks = ceil_div(ceil_div(num_inodes_ + 1, 8), bs);
  cur += sb.inode_bitmap_blocks;
  sb.block_bitmap_start = cur;
  sb.block_bitmap_blocks = ceil_div(ceil_div(nb, 8), bs);
  cur += sb.block_bitmap_blocks;
  sb.data_start = cur;

  // Zero the metadata area (the table + both bitmaps).
  for (uint32_t b = sb.inode_table_start; b < sb.data_start; ++b)
    if (auto r = dev_.zero_block(b); !r) return r;

  // Write the superblock.
  {
    std::vector<std::byte> buf(bs, std::byte{0});
    std::memcpy(buf.data(), &sb, sizeof(sb));
    if (auto r = dev_.write_block(0, buf); !r) return r;
  }

  // Block bitmap: mark the metadata area [0, data_start) as used.
  BlockAllocator balloc(dev_, sb.block_bitmap_start, sb.block_bitmap_blocks,
                        sb.data_start, nb);
  for (uint32_t b = 0; b < sb.data_start; ++b)
    if (auto r = balloc.mark_used(b); !r) return r;

  // Create the root directory inode: the inode bitmap allocates starting at
  // ROOT_INODE (0 is reserved as nil), and at format time this is the first
  // allocation, so rid must be ROOT_INODE.
  auto rid = allocate_inode(INODE_DIR);
  if (!rid) return std::unexpected(rid.error());
  return {};
}
}  // namespace nanofs
