// Inode layer (provided).
//
// Manages blocks as inodes on top of the block layer, superblock included.
//   Layout: | Super | Inode Table | Inode bitmap | Block bitmap | data blocks |
//   The inode table maps inode_id -> block_id; one inode occupies exactly one block.
//   InodeManager: allocate_inode / free_inode / get (the block an inode lives in) /
//                 set_table, plus raw inode read/write.
//
// Boundary with what you write: this layer provides format, raw inode read/write and the
// inode table. Walking indirect blocks and growing/shrinking a file's block list are in
// fs.cc (docs/lab1/lab1a.md).

#pragma once
#include <cstdint>
#include <type_traits>
#include "nanofs/block.h"
#include "nanofs/error.h"

namespace nanofs {
using inode_id = uint32_t;     // 0 = nil; valid inodes start at 1

inline constexpr uint32_t NDIRECT = 100;     // number of direct block pointers
inline constexpr inode_id ROOT_INODE = 1;
inline constexpr uint32_t INODE_FREE = 0;
inline constexpr uint32_t INODE_FILE = 1;
inline constexpr uint32_t INODE_DIR = 2;

// The on-disk inode; exactly one block (format() checks block_size >= sizeof(inode)).
// All fields are uint32: trivially copyable, serialised with memcpy.
//
// blocks[] is the classic Unix multi-level index, NDIRECT+2 slots in three parts:
//   blocks[0 .. NDIRECT-1] direct: the slot holds a data block number (first NDIRECT file blocks).
//   blocks[NDIRECT]        single indirect: points at a pointer block holding BS/4 data block numbers.
//   blocks[NDIRECT+1]      double indirect: points at a table of pointer blocks, (BS/4)^2 data blocks.
// Small files use only direct slots; a growing file reaches the indirect tiers.
// The walk itself is yours: logical_to_physical_block in fs.cc.
struct inode {
  uint32_t type;      // INODE_FREE / FILE / DIR
  uint32_t nlink;
  uint32_t size;      // bytes
  uint32_t atime;
  uint32_t mtime;
  uint32_t ctime;
  uint32_t blocks[NDIRECT + 2];   // [0..99] direct, [100] single indirect, [101] double indirect
};
static_assert(std::is_trivially_copyable_v<inode>);

// The superblock in block 0 (all uint32, serialised with memcpy).
struct Superblock {
  uint32_t magic;               // 0x6E616E6F "nano"
  uint32_t block_size;
  uint32_t num_blocks;
  uint32_t num_inodes;
  uint32_t inode_table_start;   // the inode_id -> block_id table
  uint32_t inode_table_blocks;
  uint32_t inode_bitmap_start;
  uint32_t inode_bitmap_blocks;
  uint32_t block_bitmap_start;
  uint32_t block_bitmap_blocks;
  uint32_t data_start;          // first allocatable data block
};
inline constexpr uint32_t NANOFS_MAGIC = 0x6E616E6F;

// Inode table entry: inode_id -> (structure block, generation). The table region is fixed,
// indexed by id and never reclaimed, so the generation survives free/reallocate. (Stored in
// struct inode it would be lost with the structure block; this supports remote-handle identity.)
struct InodeTableEntry {
  block_id where;        // 0 = this id is not allocated
  uint32_t generation;   // incremented on every allocation of this id; 0 = never allocated
};
static_assert(sizeof(InodeTableEntry) == 8);

// Layout + inode table + inode allocation + raw inode read/write.
// Does not interpret block pointers; that is fs.cc's job.
class InodeManager {
 public:
  InodeManager(BlockDevice& dev, uint32_t num_inodes);
  Result<void> format();                       // mkfs
  Result<inode_id> allocate_inode(uint32_t type);
  // Free id: release its structure block and clear the table entry's `where` (generation is
  // kept). Afterwards get/read_inode/write_inode on id return NotFound until it is allocated
  // again. Does not free the file's data blocks.
  Result<void> free_inode(inode_id id);
  // inode table: id -> the block its structure lives in. NotFound if id is out of range or
  // not currently allocated.
  Result<block_id> get(inode_id id);
  // Current generation of inode number id (from the table entry). 0 if never allocated;
  // NotFound if id is out of range.
  Result<uint32_t> generation(inode_id id);
  Result<inode> read_inode(inode_id id);
  Result<void> write_inode(inode_id id, const inode& node);

 private:
  Result<Superblock> read_super();
  Result<InodeTableEntry> read_entry(inode_id id);              // read a whole table entry
  Result<void> set_table(inode_id id, block_id where, uint32_t generation);  // write a table entry
  BlockDevice& dev_;
  uint32_t num_inodes_;
};
}  // namespace nanofs
