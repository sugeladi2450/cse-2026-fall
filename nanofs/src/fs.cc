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

static Result<void> free_logical_range(BlockDevice& dev, inode& node, uint32_t first,
                                       BlockAllocator& balloc, uint32_t bs) {
  uint32_t P = bs / (uint32_t)sizeof(block_id);
  if (first < NDIRECT) {
    for (uint32_t i = first; i < NDIRECT; i++) {
      if (node.blocks[i] != NULL_BLOCK) {
        if (auto r = balloc.free(node.blocks[i]); !r) return r;
        node.blocks[i] = NULL_BLOCK;
      }
    }
  }
  if (first < NDIRECT + P && node.blocks[NDIRECT] != NULL_BLOCK) {
    uint32_t start = (first > NDIRECT) ? (first - NDIRECT) : 0;
    auto ptrs = read_ptrs(dev, node.blocks[NDIRECT]);
    if (!ptrs) return std::unexpected(ptrs.error());
    bool keep = false;
    for (uint32_t i = 0; i < P; i++) {
      if (i < start) {
        if ((*ptrs)[i] != NULL_BLOCK) keep = true;
      } else if ((*ptrs)[i] != NULL_BLOCK) {
        if (auto r = balloc.free((*ptrs)[i]); !r) return r;
        (*ptrs)[i] = NULL_BLOCK;
      }
    }
    if (!keep) {
      if (auto r = balloc.free(node.blocks[NDIRECT]); !r) return r;
      node.blocks[NDIRECT] = NULL_BLOCK;
    } else {
      if (auto r = write_ptrs(dev, node.blocks[NDIRECT], *ptrs); !r) return r;
    }
  }
  if (first < NDIRECT + P + P * P && node.blocks[NDIRECT + 1] != NULL_BLOCK) {
    uint32_t dstart = (first > NDIRECT + P) ? (first - NDIRECT - P) : 0;
    auto root = read_ptrs(dev, node.blocks[NDIRECT + 1]);
    if (!root) return std::unexpected(root.error());
    bool root_keep = false;
    for (uint32_t i = 0; i < P; i++) {
      if (i < dstart / P) {
        if ((*root)[i] != NULL_BLOCK) root_keep = true;
      } else if ((*root)[i] != NULL_BLOCK) {
        auto l2 = read_ptrs(dev, (*root)[i]);
        if (!l2) return std::unexpected(l2.error());
        uint32_t sub = (i == dstart / P) ? (dstart % P) : 0;
        bool l2_keep = false;
        for (uint32_t j = 0; j < P; j++) {
          if (j < sub) {
            if ((*l2)[j] != NULL_BLOCK) l2_keep = true;
          } else if ((*l2)[j] != NULL_BLOCK) {
            if (auto r = balloc.free((*l2)[j]); !r) return r;
            (*l2)[j] = NULL_BLOCK;
          }
        }
        if (!l2_keep) {
          if (auto r = balloc.free((*root)[i]); !r) return r;
          (*root)[i] = NULL_BLOCK;
        } else {
          if (auto r = write_ptrs(dev, (*root)[i], *l2); !r) return r;
          root_keep = true;
        }
      }
    }
    if (!root_keep) {
      if (auto r = balloc.free(node.blocks[NDIRECT + 1]); !r) return r;
      node.blocks[NDIRECT + 1] = NULL_BLOCK;
    } else {
      if (auto r = write_ptrs(dev, node.blocks[NDIRECT + 1], *root); !r) return r;
    }
  }
  return {};
}

Result<block_id> FsLayer::logical_to_physical_block(inode& node, uint32_t idx,
                                                    bool allocate,
                                                    BlockAllocator& balloc,
                                                    uint32_t bs) {
  uint32_t P = bs / (uint32_t)sizeof(block_id);
  uint32_t max_idx = NDIRECT + P + P * P - 1;
  if (idx > max_idx) return std::unexpected(Error::Invalid);

  auto alloc_zero = [&]() -> Result<block_id> {
    auto b = balloc.allocate();
    if (!b) return std::unexpected(b.error());
    if (auto r = dev_.zero_block(*b); !r) {
      (void)balloc.free(*b);
      return std::unexpected(r.error());
    }
    return *b;
  };

  if (idx < NDIRECT) {
    if (node.blocks[idx] != NULL_BLOCK || !allocate) return node.blocks[idx];
    auto b = alloc_zero();
    if (!b) return std::unexpected(b.error());
    node.blocks[idx] = *b;
    return *b;
  }

  if (idx < NDIRECT + P) {
    uint32_t slot = idx - NDIRECT;
    if (node.blocks[NDIRECT] == NULL_BLOCK) {
      if (!allocate) return NULL_BLOCK;
      auto ib = alloc_zero();
      if (!ib) return std::unexpected(ib.error());
      node.blocks[NDIRECT] = *ib;
    }
    auto ptrs = read_ptrs(dev_, node.blocks[NDIRECT]);
    if (!ptrs) return std::unexpected(ptrs.error());
    if ((*ptrs)[slot] != NULL_BLOCK) return (*ptrs)[slot];
    if (!allocate) return NULL_BLOCK;
    auto b = alloc_zero();
    if (!b) return std::unexpected(b.error());
    (*ptrs)[slot] = *b;
    if (auto r = write_ptrs(dev_, node.blocks[NDIRECT], *ptrs); !r)
      return std::unexpected(r.error());
    return *b;
  }

  uint32_t d = idx - NDIRECT - P;
  uint32_t l1 = d / P;
  uint32_t l2 = d % P;

  if (node.blocks[NDIRECT + 1] == NULL_BLOCK) {
    if (!allocate) return NULL_BLOCK;
    auto root = alloc_zero();
    if (!root) return std::unexpected(root.error());
    node.blocks[NDIRECT + 1] = *root;
  }
  auto lvl1 = read_ptrs(dev_, node.blocks[NDIRECT + 1]);
  if (!lvl1) return std::unexpected(lvl1.error());
  if ((*lvl1)[l1] == NULL_BLOCK) {
    if (!allocate) return NULL_BLOCK;
    auto blk2 = alloc_zero();
    if (!blk2) return std::unexpected(blk2.error());
    (*lvl1)[l1] = *blk2;
    if (auto r = write_ptrs(dev_, node.blocks[NDIRECT + 1], *lvl1); !r)
      return std::unexpected(r.error());
  }
  auto lvl2 = read_ptrs(dev_, (*lvl1)[l1]);
  if (!lvl2) return std::unexpected(lvl2.error());
  if ((*lvl2)[l2] != NULL_BLOCK) return (*lvl2)[l2];
  if (!allocate) return NULL_BLOCK;
  auto b = alloc_zero();
  if (!b) return std::unexpected(b.error());
  (*lvl2)[l2] = *b;
  if (auto r = write_ptrs(dev_, (*lvl1)[l1], *lvl2); !r) return std::unexpected(r.error());
  return *b;
}

Result<std::string> FsLayer::read_bytes(inode& node, uint32_t off, uint32_t len) {
  if (off >= node.size) return std::string("");
  uint32_t end = std::min(off + len, node.size);
  uint32_t n = end - off;

  auto sb = super();
  if (!sb) return std::unexpected(sb.error());
  uint32_t bs = sb->block_size;
  BlockAllocator balloc(dev_, sb->block_bitmap_start, sb->block_bitmap_blocks,
                        sb->data_start, sb->num_blocks);

  std::string out;
  out.reserve(n);
  uint32_t cur = off;
  while (cur < end) {
    uint32_t logical = cur / bs;
    uint32_t pos = cur % bs;
    uint32_t chunk = std::min(bs - pos, end - cur);
    auto phys = logical_to_physical_block(node, logical, false, balloc, bs);
    if (!phys) return std::unexpected(phys.error());
    if (*phys == NULL_BLOCK) {
      out.append(chunk, '\0');
    } else {
      std::vector<std::byte> buf(bs);
      if (auto r = dev_.read_block(*phys, buf); !r) return std::unexpected(r.error());
      out.append(reinterpret_cast<const char*>(buf.data()) + pos, chunk);
    }
    cur += chunk;
  }
  return out;
}

// TODO Lab2A: classify writes here as BlockKind::Data for regular-file
// contents and BlockKind::Meta for directory contents.
Result<uint32_t> FsLayer::write_bytes(inode_id id, inode& node, uint32_t off,
                                      const std::string& data) {
  auto sb = super();
  if (!sb) return std::unexpected(sb.error());
  uint32_t bs = sb->block_size;
  BlockAllocator balloc(dev_, sb->block_bitmap_start, sb->block_bitmap_blocks,
                        sb->data_start, sb->num_blocks);

  uint32_t written = 0;
  uint32_t cur = off;
  while (written < data.size()) {
    uint32_t logical = cur / bs;
    uint32_t pos = cur % bs;
    uint32_t chunk = std::min(bs - pos, (uint32_t)(data.size() - written));
    auto phys = logical_to_physical_block(node, logical, true, balloc, bs);
    if (!phys) return std::unexpected(phys.error());

    std::vector<std::byte> buf(bs, std::byte{0});
    if (pos != 0 || chunk < bs) {
      if (auto r = dev_.read_block(*phys, buf); !r) return std::unexpected(r.error());
    }
    std::memcpy(buf.data() + pos, data.data() + written, chunk);
    BlockKind kind = (node.type == INODE_FILE) ? BlockKind::Data : BlockKind::Meta;
    if (auto r = dev_.write_block(*phys, buf, kind); !r) return std::unexpected(r.error());
    cur += chunk;
    written += chunk;
  }

  uint32_t new_end = off + (uint32_t)data.size();
  if (new_end > node.size) node.size = new_end;
  if (auto r = inodes_.write_inode(id, node); !r) return std::unexpected(r.error());
  return (uint32_t)data.size();
}

Result<Attr> FsLayer::getattr(inode_id id) {
  auto node = inodes_.read_inode(id);
  if (!node) return std::unexpected(node.error());
  return Attr{id, node->type == INODE_DIR, node->size};
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

Result<std::vector<DirEntry>> FsLayer::read_dir_entries(inode_id dir) {
  auto node = inodes_.read_inode(dir);
  if (!node) return std::unexpected(node.error());
  if (node->type != INODE_DIR) return std::unexpected(Error::NotDir);
  auto data = read_bytes(*node, 0, node->size);
  if (!data) return std::unexpected(data.error());
  std::vector<DirEntry> out;
  size_t n = data->size() / sizeof(DirEntry);
  for (size_t i = 0; i < n; i++) {
    DirEntry e;
    std::memcpy(&e, data->data() + i * sizeof(DirEntry), sizeof(DirEntry));
    if (e.inode_id != 0) out.push_back(e);
  }
  return out;
}

Result<inode_id> FsLayer::lookup(inode_id parent, const std::string& name) {
  auto entries = read_dir_entries(parent);
  if (!entries) return std::unexpected(entries.error());
  for (auto& e : *entries) {
    if (std::string(e.name) == name) return e.inode_id;
  }
  return std::unexpected(Error::NotFound);
}

Result<std::vector<DirEntry>> FsLayer::readdir(inode_id dir) {
  return read_dir_entries(dir);
}

Result<void> FsLayer::add_dir_entry(inode_id dir, const std::string& name,
                                    inode_id target) {
  if (name.size() >= NANOFS_NAME_MAX) return std::unexpected(Error::Invalid);
  auto node = inodes_.read_inode(dir);
  if (!node) return std::unexpected(node.error());
  auto data = read_bytes(*node, 0, node->size);
  if (!data) return std::unexpected(data.error());

  DirEntry entry{};
  entry.inode_id = target;
  std::memset(entry.name, 0, sizeof(entry.name));
  std::memcpy(entry.name, name.c_str(), name.size());

  size_t n = data->size() / sizeof(DirEntry);
  for (size_t i = 0; i < n; i++) {
    DirEntry e;
    std::memcpy(&e, data->data() + i * sizeof(DirEntry), sizeof(DirEntry));
    if (e.inode_id == 0) {
      std::string merged = *data;
      std::memcpy(&merged[i * sizeof(DirEntry)], &entry, sizeof(DirEntry));
      auto w = write_bytes(dir, *node, 0, merged);
      if (!w) return std::unexpected(w.error());
      return {};
    }
  }
  std::string blob(reinterpret_cast<const char*>(&entry), sizeof(DirEntry));
  auto w = write_bytes(dir, *node, node->size, blob);
  if (!w) return std::unexpected(w.error());
  return {};
}

Result<void> FsLayer::remove_dir_entry(inode_id dir, const std::string& name) {
  auto node = inodes_.read_inode(dir);
  if (!node) return std::unexpected(node.error());
  auto data = read_bytes(*node, 0, node->size);
  if (!data) return std::unexpected(data.error());
  size_t n = data->size() / sizeof(DirEntry);
  for (size_t i = 0; i < n; i++) {
    DirEntry e;
    std::memcpy(&e, data->data() + i * sizeof(DirEntry), sizeof(DirEntry));
    if (e.inode_id != 0 && std::string(e.name) == name) {
      std::string merged = *data;
      std::memset(&merged[i * sizeof(DirEntry)], 0, sizeof(DirEntry));
      auto w = write_bytes(dir, *node, 0, merged);
      if (!w) return std::unexpected(w.error());
      return {};
    }
  }
  return {};
}

// TODO Lab2B: keep this operation inside its existing parent lock; avoid
// re-entering LockedFs through a virtual lookup.
Result<inode_id> FsLayer::make_node(inode_id parent, const std::string& name,
                                    uint32_t type) {
  auto pnode = inodes_.read_inode(parent);
  if (!pnode) return std::unexpected(pnode.error());
  if (pnode->type != INODE_DIR) return std::unexpected(Error::NotDir);
  if (name.size() >= NANOFS_NAME_MAX) return std::unexpected(Error::Invalid);

  auto existing = FsLayer::lookup(parent, name);
  if (existing) return *existing;

  auto id = inodes_.allocate_inode(type);
  if (!id) return std::unexpected(id.error());

  auto r = add_dir_entry(parent, name, *id);
  if (!r) {
    (void)inodes_.free_inode(*id);
    return std::unexpected(r.error());
  }
  return *id;
}

Result<inode_id> FsLayer::create(inode_id parent, const std::string& name) {
  return make_node(parent, name, INODE_FILE);
}

Result<inode_id> FsLayer::mkdir(inode_id parent, const std::string& name) {
  return make_node(parent, name, INODE_DIR);
}

Result<void> FsLayer::free_file_blocks(inode& node, BlockAllocator& balloc,
                                       uint32_t bs) {
  auto r = free_logical_range(dev_, node, 0, balloc, bs);
  if (!r) return r;
  node.size = 0;
  return {};
}

// TODO Lab2B: avoid re-entering LockedFs for this internal lookup.
Result<void> FsLayer::unlink(inode_id parent, const std::string& name) {
  auto pnode = inodes_.read_inode(parent);
  if (!pnode) return std::unexpected(pnode.error());
  if (pnode->type != INODE_DIR) return std::unexpected(Error::NotDir);

  auto target = FsLayer::lookup(parent, name);
  if (!target) return {};

  auto tnode = inodes_.read_inode(*target);
  if (!tnode) return std::unexpected(tnode.error());
  if (tnode->type == INODE_DIR) return std::unexpected(Error::IsDir);

  if (auto r = remove_dir_entry(parent, name); !r) return r;

  auto sb = super();
  if (!sb) return std::unexpected(sb.error());
  uint32_t bs = sb->block_size;
  BlockAllocator balloc(dev_, sb->block_bitmap_start, sb->block_bitmap_blocks,
                        sb->data_start, sb->num_blocks);
  if (auto r = free_file_blocks(*tnode, balloc, bs); !r) return r;
  return inodes_.free_inode(*target);
}

// TODO Lab2B: avoid re-entering LockedFs for this internal lookup.
Result<void> FsLayer::rmdir(inode_id parent, const std::string& name) {
  auto pnode = inodes_.read_inode(parent);
  if (!pnode) return std::unexpected(pnode.error());
  if (pnode->type != INODE_DIR) return std::unexpected(Error::NotDir);

  auto target = FsLayer::lookup(parent, name);
  if (!target) return {};

  auto tnode = inodes_.read_inode(*target);
  if (!tnode) return std::unexpected(tnode.error());
  if (tnode->type != INODE_DIR) return std::unexpected(Error::NotDir);

  auto entries = read_dir_entries(*target);
  if (!entries) return std::unexpected(entries.error());
  if (!entries->empty()) return std::unexpected(Error::NotEmpty);

  if (auto r = remove_dir_entry(parent, name); !r) return r;

  auto sb = super();
  if (!sb) return std::unexpected(sb.error());
  uint32_t bs = sb->block_size;
  BlockAllocator balloc(dev_, sb->block_bitmap_start, sb->block_bitmap_blocks,
                        sb->data_start, sb->num_blocks);
  if (auto r = free_file_blocks(*tnode, balloc, bs); !r) return r;
  return inodes_.free_inode(*target);
}

Result<Attr> FsLayer::setattr(inode_id id, uint32_t new_size) {
  auto node = inodes_.read_inode(id);
  if (!node) return std::unexpected(node.error());
  if (node->type == INODE_DIR) return std::unexpected(Error::IsDir);

  auto sb = super();
  if (!sb) return std::unexpected(sb.error());
  uint32_t bs = sb->block_size;
  BlockAllocator balloc(dev_, sb->block_bitmap_start, sb->block_bitmap_blocks,
                        sb->data_start, sb->num_blocks);

  if (new_size < node->size) {
    uint32_t first_free = (new_size > 0) ? ((new_size - 1) / bs + 1) : 0;
    auto r = free_logical_range(dev_, *node, first_free, balloc, bs);
    if (!r) return std::unexpected(r.error());
    if (new_size > 0 && new_size % bs != 0) {
      uint32_t last_logical = (new_size - 1) / bs;
      auto phys = logical_to_physical_block(*node, last_logical, false, balloc, bs);
      if (!phys) return std::unexpected(phys.error());
      if (*phys != NULL_BLOCK) {
        std::vector<std::byte> buf(bs);
        if (auto rd = dev_.read_block(*phys, buf); !rd) return std::unexpected(rd.error());
        uint32_t tail = new_size % bs;
        std::memset(buf.data() + tail, 0, bs - tail);
        if (auto wr = dev_.write_block(*phys, buf, BlockKind::Data); !wr) return std::unexpected(wr.error());
      }
    }
  }

  node->size = new_size;
  if (auto w = inodes_.write_inode(id, *node); !w) return std::unexpected(w.error());
  return Attr{id, node->type == INODE_DIR, new_size};
}
}  // namespace nanofs
