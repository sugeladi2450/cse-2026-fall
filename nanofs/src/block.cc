#include "nanofs/block.h"
#include "nanofs/allocator_lock.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <filesystem>
#include <fstream>

namespace nanofs {

SimulatedBlockDevice::SimulatedBlockDevice(size_t block_size, size_t num_blocks)
    : block_size_(block_size),
      num_blocks_(num_blocks),
      mem_(block_size * num_blocks, std::byte{0}) {}

Result<std::unique_ptr<SimulatedBlockDevice>> SimulatedBlockDevice::open_file(
    std::string path, size_t block_size, size_t num_blocks) {
  namespace fs = std::filesystem;
  const size_t total = block_size * num_blocks;
  std::error_code ec;
  bool exists = fs::exists(path, ec);
  if (ec) return std::unexpected(Error::IoError);
  if (!exists) {
    std::ofstream create(path, std::ios::binary);
    if (!create) return std::unexpected(Error::IoError);
  }
  fs::resize_file(path, total, ec);
  if (ec) return std::unexpected(Error::IoError);
  auto dev = std::make_unique<SimulatedBlockDevice>(block_size, num_blocks);
  std::vector<std::byte>().swap(dev->mem_);  // release the capacity; the file backend keeps no in-memory copy
  dev->file_.emplace(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!dev->file_->is_open()) return std::unexpected(Error::IoError);
  return dev;
}

// A normal destructor is a clean close (not a crash): un-flushed writes are written back,
// and a crash_at_flush that never fired is cancelled. Errors are ignored: a destructor cannot throw.
SimulatedBlockDevice::~SimulatedBlockDevice() { crash_nth_ = 0; (void)flush(); }

// Write straight to the underlying medium (memory or file), bypassing the
// block cache and without flushing the stream — flush() does that once for all
// writes. Used internally by write_block/flush.
Result<void> SimulatedBlockDevice::durable_write(block_id id, std::span<const std::byte> buf) {
  if (file_) {
    file_->seekp(static_cast<std::streamoff>(id) * block_size_);
    file_->write(reinterpret_cast<const char*>(buf.data()), block_size_);
    if (!*file_) { file_->clear(); return std::unexpected(Error::IoError); }
    return {};
  }
  std::memcpy(mem_.data() + id * block_size_, buf.data(), block_size_);
  return {};
}

Result<void> SimulatedBlockDevice::read_block(block_id id, std::span<std::byte> buf) {
  if (buf.size() != block_size_) return std::unexpected(Error::Invalid);
  if (id >= num_blocks_) return std::unexpected(Error::IoError);
  // A read on the memory backend is a pure read -> take the shared lock, so
  // concurrent device reads do not block each other.
  // The file backend has to move the fstream's position (seekg), which is a
  // mutation -> exclusive lock only.
  if (file_) {
    std::unique_lock<std::shared_mutex> lk(cache_mutex_);
    return read_locked(id, buf);
  }
  std::shared_lock<std::shared_mutex> lk(cache_mutex_);
  return read_locked(id, buf);
}

Result<void> SimulatedBlockDevice::read_locked(block_id id, std::span<std::byte> buf) {
  if (auto it = block_cache_.find(id); it != block_cache_.end()) {
    std::memcpy(buf.data(), it->second.data(), block_size_);   // hit in the volatile layer
    return {};
  }
  // Miss: read the medium directly and do NOT fill the block cache
  // (write-back only; see the field comment in block.h).
  if (file_) {
    file_->seekg(static_cast<std::streamoff>(id) * block_size_);
    file_->read(reinterpret_cast<char*>(buf.data()), block_size_);
    if (!*file_) { file_->clear(); return std::unexpected(Error::IoError); }
    return {};
  }
  std::memcpy(buf.data(), mem_.data() + id * block_size_, block_size_);
  return {};
}

Result<void> SimulatedBlockDevice::write_block(block_id id, std::span<const std::byte> buf,
                                               BlockKind /*kind*/) {
  if (buf.size() != block_size_) return std::unexpected(Error::Invalid);
  if (id >= num_blocks_) return std::unexpected(Error::IoError);
  std::unique_lock<std::shared_mutex> lk(cache_mutex_);
  if (power_off_) return std::unexpected(Error::IoError);   // dead until reboot()
  block_cache_[id].assign(buf.begin(), buf.end());  // into the volatile layer; only flush persists it (no journal here: kind does not matter)
  return {};
}

Result<void> SimulatedBlockDevice::zero_block(block_id id) {
  if (id >= num_blocks_) return std::unexpected(Error::IoError);
  std::unique_lock<std::shared_mutex> lk(cache_mutex_);
  if (power_off_) return std::unexpected(Error::IoError);   // dead until reboot()
  block_cache_[id].assign(block_size_, std::byte{0});
  return {};
}

// Write every un-flushed block in the block cache to the durable medium, then drain the
// stream **once** (this is fsync: N random block writes cost one sync), then clear the
// cache. Note std::fstream::flush only pushes data to the OS page cache (a different thing
// from this device's block cache), not a real fsync to disk; the crash semantics of this
// simulated device (reboot() drops only the block cache, the file counts as persistent)
// do not need a real fsync, so no raw fd is involved.
// If crash_at_flush fires on this flush: only the first k blocks land, the rest are lost
// with the cache, IoError is returned, and the whole machine is powered off (every
// device's writes / flushes fail) until reboot().
Result<void> SimulatedBlockDevice::flush() {
  std::unique_lock<std::shared_mutex> lk(cache_mutex_);
  if (power_off_) return std::unexpected(Error::IoError);   // dead until reboot()

  // crash_at_flush(nth, k) names the nth flush from now: count down on every flush.
  const bool armed_flush = crash_nth_ > 0 && --crash_nth_ == 0;
  // A flush with N dirty blocks has N+1 places to lose power (0..N blocks landed). k > N
  // means the test has enumerated them all: complete normally, crashed() stays false.
  const bool crash = armed_flush && crash_k_ <= block_cache_.size();
  const size_t n_land = crash ? crash_k_ : block_cache_.size();   // how many dirty blocks reach the medium

  size_t i = 0;
  for (auto& [id, buf] : block_cache_) {
    if (i++ == n_land) break;
    if (auto r = durable_write(id, buf); !r) return r;
  }
  if (file_) {
    file_->flush();
    if (!*file_) { file_->clear(); return std::unexpected(Error::IoError); }
  }
  block_cache_.clear();   // landed or lost, nothing stays dirty

  if (crash) {
    crashed_ = true;
    power_off_ = true;
    return std::unexpected(Error::IoError);
  }
  return {};
}

// Power loss + restart: drop the block cache (un-flushed writes), durable medium untouched;
// if the machine is powered off because crash_at_flush fired, power it back on.
void SimulatedBlockDevice::reboot() {
  std::unique_lock<std::shared_mutex> lk(cache_mutex_);
  block_cache_.clear();
  power_off_ = false;
}

BlockAllocator::BlockAllocator(BlockDevice& dev, block_id bitmap_start,
                               block_id bitmap_blocks, block_id managed_begin,
                               block_id managed_end)
    : dev_(dev), bitmap_start_(bitmap_start), bitmap_blocks_(bitmap_blocks),
      managed_begin_(managed_begin), managed_end_(managed_end) {}

Result<void> BlockAllocator::format() {
  for (block_id i = 0; i < bitmap_blocks_; ++i)
    if (auto r = dev_.zero_block(bitmap_start_ + i); !r) return r;
  return {};
}

// Read/write bit id of the bitmap. The bit's byte offset within the bitmap
// area = id/8; then locate that byte inside its block.
Result<bool> BlockAllocator::get_bit(block_id id) {
  const size_t bs = dev_.block_size();
  const size_t byte_off = id / 8;
  const block_id blk = bitmap_start_ + static_cast<block_id>(byte_off / bs);
  const size_t in_blk = byte_off % bs;
  std::vector<std::byte> buf(bs);
  if (auto r = dev_.read_block(blk, buf); !r) return std::unexpected(r.error());
  return (std::to_integer<unsigned>(buf[in_blk]) >> (id % 8)) & 1u;
}

// Read-modify-write: set bit id of the bitmap to value (keeping the other
// bits of the same byte).
Result<void> BlockAllocator::set_bit(block_id id, bool value) {
  const size_t bs = dev_.block_size();
  const size_t byte_off = id / 8;
  const block_id blk = bitmap_start_ + static_cast<block_id>(byte_off / bs);
  const size_t in_blk = byte_off % bs;
  std::vector<std::byte> buf(bs);
  if (auto r = dev_.read_block(blk, buf); !r) return r;
  unsigned b = std::to_integer<unsigned>(buf[in_blk]);
  if (value) b |= (1u << (id % 8)); else b &= ~(1u << (id % 8));
  buf[in_blk] = std::byte{static_cast<unsigned char>(b)};
  return dev_.write_block(blk, buf);
}

Result<block_id> BlockAllocator::allocate() {
  AllocatorLock::engage_current();
  for (block_id id = managed_begin_; id < managed_end_; ++id) {
    auto used = get_bit(id);
    if (!used) return std::unexpected(used.error());
    if (!*used) { if (auto r = set_bit(id, true); !r) return std::unexpected(r.error());
                  return id; }
  }
  return std::unexpected(Error::NoSpace);
}

Result<void> BlockAllocator::free(block_id id) {
  AllocatorLock::engage_current();
  return set_bit(id, false);
}
Result<void> BlockAllocator::mark_used(block_id id) { return set_bit(id, true); }
Result<bool> BlockAllocator::is_allocated(block_id id) { return get_bit(id); }
}  // namespace nanofs
