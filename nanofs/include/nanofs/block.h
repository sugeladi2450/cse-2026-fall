#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>
#include "nanofs/error.h"

namespace nanofs {
using block_id = uint32_t;

// Block 0 is the superblock and allocation starts at data_start, so 0 is never a valid
// data block. In an inode's blocks[] it means "this logical block is not allocated" (a hole).
inline constexpr block_id NULL_BLOCK = 0;

// Write classification: metadata or content. The provided device treats both alike.
enum class BlockKind { Meta, Data };

// The lowest-level block device interface: whole-block I/O, pluggable backend.
// The inode and fs layers depend only on this and never see the backend.
class BlockDevice {
 public:
  virtual ~BlockDevice() = default;
  // Read block id into buf. buf.size() must equal block_size() (else Error::Invalid);
  // an out-of-range id is Error::IoError.
  virtual Result<void> read_block(block_id id, std::span<std::byte> buf) = 0;
  // Write block id. Same size and range rules as read_block. kind says whether the block
  // is metadata or file content; the provided device treats both alike.
  virtual Result<void> write_block(block_id id, std::span<const std::byte> buf,
                                   BlockKind kind = BlockKind::Meta) = 0;
  // Zero block id.
  virtual Result<void> zero_block(block_id id) = 0;
  virtual size_t num_blocks() const = 0;
  virtual size_t block_size() const = 0;
  // Persist buffered writes to the underlying medium.
  virtual Result<void> flush() = 0;
  // Test hook: what a power loss + restart does to this device — un-flushed writes are
  // gone, the medium is untouched. No-op by default.
  virtual void reboot() {}
};

// Simulated block device with two backends: in-memory, or a file image.
class SimulatedBlockDevice : public BlockDevice {
 public:
  SimulatedBlockDevice(size_t block_size, size_t num_blocks);   // in-memory backend
  // File-image backend: open or create path, sized to block_size * num_blocks.
  static Result<std::unique_ptr<SimulatedBlockDevice>> open_file(
      std::string path, size_t block_size, size_t num_blocks);
  // A normal destructor writes the block cache back: a clean shutdown is durable.
  // Only reboot() (power loss) drops un-flushed writes.
  ~SimulatedBlockDevice() override;

  Result<void> read_block(block_id id, std::span<std::byte> buf) override;
  Result<void> write_block(block_id id, std::span<const std::byte> buf,
                           BlockKind kind = BlockKind::Meta) override;
  Result<void> zero_block(block_id id) override;
  size_t num_blocks() const override { return num_blocks_; }
  size_t block_size() const override { return block_size_; }
  Result<void> flush() override;
  void reboot() override;
  // Test hook: lose power inside the nth flush() from now (1-based). That flush persists
  // only the first k dirty blocks (in block-id order), loses the rest, and returns
  // Error::IoError. k == dirty count: power was lost right after a complete flush.
  // k > dirty count: the flush completes normally and crashed() stays false — that is
  // how a test knows it has enumerated every crash point of that flush.
  //
  // Power loss is machine-wide (one process = one machine): from then on every
  // SimulatedBlockDevice is dead (fail-stop) until reboot() turns the power back on.
  // write_block / zero_block / flush return Error::IoError, so nothing more can reach
  // any medium.
  void crash_at_flush(uint32_t nth, size_t k) { crash_nth_ = nth; crash_k_ = k; crashed_ = false; }
  bool crashed() const { return crashed_; }

 private:
  size_t block_size_;
  size_t num_blocks_;
  uint32_t crash_nth_ = 0;   // flushes left until the armed power loss; 0 = not armed
  size_t crash_k_ = 0;
  bool crashed_ = false;     // this device's flush is where power was lost
  static inline std::atomic<bool> power_off_ = false;   // machine-wide: set by crash_at_flush firing, cleared by reboot()
  std::vector<std::byte> mem_;        // in-memory backend
  std::optional<std::fstream> file_;  // file backend (present = file mode)
  // Un-flushed writes (the volatile layer). Write-back only: writes fill it, reads hit it,
  // but a read miss does not populate it, and there is no capacity limit or eviction.
  // Dirty blocks reach the medium only in flush(), so the crash point is entirely decided
  // by flush() (eviction would let dirty blocks trickle out and make crashes unpredictable).
  std::map<block_id, std::vector<std::byte>> block_cache_;
  // The block device protects block_cache_ and mem_/file_
  // are guarded: reads hold it shared, mutations (write/zero/flush/crash) hold it exclusive.
  // Without it one thread inserting into the map during commit while another iterates it
  // corrupts memory.
  mutable std::shared_mutex cache_mutex_;
  Result<void> durable_write(block_id id, std::span<const std::byte> buf);  // straight to mem_/file_
  Result<void> read_locked(block_id id, std::span<std::byte> buf);          // caller holds cache_mutex_
};

// Bitmap block allocator. The bitmap lives on the device; bit i is block i.
// allocate() only hands out blocks in [managed_begin, managed_end).
class BlockAllocator {
 public:
  // Manages the bitmap stored in [bitmap_start, bitmap_start + bitmap_blocks) and allocates
  // from [managed_begin, managed_end). Holds a non-owning BlockDevice&.
  BlockAllocator(BlockDevice& dev, block_id bitmap_start, block_id bitmap_blocks,
                 block_id managed_begin, block_id managed_end);
  Result<void> format();                 // zero the bitmap (everything free)
  // First free block in [managed_begin, managed_end), marked used; Error::NoSpace if none.
  Result<block_id> allocate();
  // Clear id's bit (mark it free).
  Result<void> free(block_id id);
  Result<void> mark_used(block_id id);   // force the bit set (format() reserves the metadata region with it)
  // Whether id's bit is set (allocated).
  Result<bool> is_allocated(block_id id);

 private:
  Result<void> set_bit(block_id id, bool value);
  Result<bool> get_bit(block_id id);
  BlockDevice& dev_;
  block_id bitmap_start_, bitmap_blocks_, managed_begin_, managed_end_;
};
}  // namespace nanofs
