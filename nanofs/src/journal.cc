#include "nanofs/journal.h"
#include "nanofs/log.h"
#include <cstring>

namespace nanofs {

namespace {
uint32_t checksum_records(
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
  if (!create_tx()) return std::unexpected(Error::Invalid);
  return {};
}

Result<void> JournaledBlockDevice::abort() {
  erase_tx();
  return {};
}

Result<void> JournaledBlockDevice::write_block(block_id id,
                                               std::span<const std::byte> buf,
                                               BlockKind kind) {
  if (buf.size() != block_size()) return std::unexpected(Error::Invalid);
  if (id >= image_.num_blocks()) return std::unexpected(Error::IoError);

  if (kind == BlockKind::Data) {
    auto* tx = current_tx();
    if (tx) tx->erase(id);
    return image_.write_block(id, buf, kind);
  }

  auto* tx = current_tx();
  if (!tx) return image_.write_block(id, buf, kind);
  (*tx)[id] = std::vector<std::byte>(buf.begin(), buf.end());
  return {};
}

Result<void> JournaledBlockDevice::read_block(block_id id,
                                              std::span<std::byte> buf) {
  if (buf.size() != block_size()) return std::unexpected(Error::Invalid);
  if (id >= image_.num_blocks()) return std::unexpected(Error::IoError);

  auto* tx = current_tx();
  if (tx) {
    auto it = tx->find(id);
    if (it != tx->end()) {
      std::memcpy(buf.data(), it->second.data(), buf.size());
      return {};
    }
  }
  return image_.read_block(id, buf);
}

Result<void> JournaledBlockDevice::zero_block(block_id id) {
  if (id >= image_.num_blocks()) return std::unexpected(Error::IoError);
  auto* tx = current_tx();
  if (!tx) return image_.zero_block(id);
  (*tx)[id] = std::vector<std::byte>(block_size(), std::byte{0});
  return {};
}

Result<void> JournaledBlockDevice::commit() {
  std::lock_guard<std::mutex> g(journal_lock_);
  auto* tx = current_tx();
  if (!tx) return {};
  WriteSet ws = *tx;
  erase_tx();

  if (ws.empty()) return {};

  size_t bs = block_size();
  uint32_t max_records = (uint32_t)((bs - sizeof(JournalDesc)) / sizeof(block_id));
  if (ws.size() > max_records) return std::unexpected(Error::NoSpace);

  uint32_t n = (uint32_t)ws.size();

  if (cursor_ + 1 + n > journal_.num_blocks()) {
    if (auto r = checkpoint(); !r) return r;
  }

  std::vector<std::vector<std::byte>> records;
  std::vector<block_id> ids;
  records.reserve(n);
  ids.reserve(n);
  for (auto& [id, data] : ws) {
    ids.push_back(id);
    records.push_back(data);
  }

  uint32_t cksum = checksum_records(records);

  for (uint32_t i = 0; i < n; i++) {
    if (auto r = journal_.write_block(cursor_ + 1 + i, records[i]); !r) return r;
  }

  std::vector<std::byte> desc(bs, std::byte{0});
  JournalDesc jd;
  jd.magic = NANOFS_JRNL_MAGIC;
  jd.n_records = n;
  jd.records_checksum = cksum;
  std::memcpy(desc.data(), &jd, sizeof(jd));
  std::memcpy(desc.data() + sizeof(jd), ids.data(), n * sizeof(block_id));

  if (auto r = journal_.write_block(cursor_, desc); !r) return r;

  if (auto r = journal_.flush(); !r) return r;

  cursor_ += 1 + n;

  for (auto& [id, data] : ws) {
    if (auto r = image_.write_block(id, data); !r) break;
  }

  if (cursor_ >= checkpoint_threshold_) {
    (void)checkpoint();
  }

  return {};
}

Result<void> JournaledBlockDevice::reset_journal() {
  std::vector<std::byte> zeros(block_size(), std::byte{0});
  for (block_id i = 0; i < (block_id)journal_.num_blocks(); i++) {
    if (auto r = journal_.write_block(i, zeros); !r) return r;
  }
  if (auto r = journal_.flush(); !r) return r;
  cursor_ = 0;
  return {};
}

Result<void> JournaledBlockDevice::checkpoint() {
  if (auto r = image_.flush(); !r) return r;
  return reset_journal();
}

Result<void> JournaledBlockDevice::recover() {
  block_id cur = 0;
  bool replayed = false;
  size_t bs = block_size();

  while (cur < (block_id)journal_.num_blocks()) {
    std::vector<std::byte> desc(bs);
    auto rd = journal_.read_block(cur, desc);
    if (!rd) return std::unexpected(rd.error());

    JournalDesc jd;
    std::memcpy(&jd, desc.data(), sizeof(jd));

    if (jd.magic != NANOFS_JRNL_MAGIC) break;
    if (jd.n_records == 0) break;
    if (cur + 1 + jd.n_records > (block_id)journal_.num_blocks()) break;

    std::vector<std::vector<std::byte>> records;
    std::vector<block_id> ids;
    records.reserve(jd.n_records);
    ids.reserve(jd.n_records);
    for (uint32_t i = 0; i < jd.n_records; i++) {
      std::vector<std::byte> rec(bs);
      auto r = journal_.read_block(cur + 1 + i, rec);
      if (!r) return std::unexpected(r.error());
      records.push_back(rec);
      block_id id;
      std::memcpy(&id, desc.data() + sizeof(jd) + i * sizeof(block_id), sizeof(block_id));
      ids.push_back(id);
    }

    if (checksum_records(records) != jd.records_checksum) break;

    for (uint32_t i = 0; i < jd.n_records; i++) {
      if (auto r = image_.write_block(ids[i], records[i]); !r) {
        return std::unexpected(r.error());
      }
    }
    replayed = true;

    cur += 1 + jd.n_records;
  }

  if (replayed) {
    if (auto r = image_.flush(); !r) return r;
  }

  return reset_journal();
}

}  // namespace nanofs
