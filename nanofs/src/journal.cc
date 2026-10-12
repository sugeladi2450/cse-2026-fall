#include "nanofs/journal.h"
#include "nanofs/log.h"
#include <cstring>

namespace nanofs {

namespace {
// FNV-1a over the bytes of every record block; detects a torn tail of a
// multi-block journal write.
[[maybe_unused]] uint32_t checksum_records(
    const std::vector<std::vector<std::byte>>& recs) {
  uint32_t h = 2166136261u;
  for (const auto& r : recs)
    for (std::byte b : r) {
      h ^= std::to_integer<uint32_t>(b);
      h *= 16777619u;
    }
  return h;
}
}  // namespace

// The transaction-map helpers are provided. Students implement the public
// journal operations below according to journal.h and docs/lab2/lab2a.md.
JournaledBlockDevice::WriteSet* JournaledBlockDevice::current_tx() {
  std::lock_guard<std::mutex> g(tx_map_mutex_);
  auto it = txs_.find(std::this_thread::get_id());
  return it == txs_.end() ? nullptr : &it->second;
}

void JournaledBlockDevice::erase_tx() {
  std::lock_guard<std::mutex> g(tx_map_mutex_);
  txs_.erase(std::this_thread::get_id());
}

bool JournaledBlockDevice::create_tx() {
  std::lock_guard<std::mutex> g(tx_map_mutex_);
  return txs_.try_emplace(std::this_thread::get_id()).second;
}

Result<void> JournaledBlockDevice::begin() {
  /* TODO Lab2A: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<void> JournaledBlockDevice::abort() {
  /* TODO Lab2A: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<void> JournaledBlockDevice::write_block(block_id id,
                                               std::span<const std::byte> buf,
                                               BlockKind kind) {
  /* TODO Lab2A: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<void> JournaledBlockDevice::read_block(block_id id,
                                              std::span<std::byte> buf) {
  /* TODO Lab2A: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<void> JournaledBlockDevice::zero_block(block_id id) {
  /* TODO Lab2A: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<void> JournaledBlockDevice::commit() {
  /* TODO Lab2A: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<void> JournaledBlockDevice::reset_journal() {
  /* TODO Lab2A: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<void> JournaledBlockDevice::checkpoint() {
  /* TODO Lab2A: Your code here */
  return std::unexpected(Error::Invalid);
}

Result<void> JournaledBlockDevice::recover() {
  /* TODO Lab2A: Your code here */
  return std::unexpected(Error::Invalid);
}

}  // namespace nanofs
