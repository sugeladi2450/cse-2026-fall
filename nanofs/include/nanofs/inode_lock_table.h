#pragma once
#include <cstdint>
#include <mutex>  // std::unique_lock (not transitively provided by <shared_mutex> on libstdc++ 12+)
#include <shared_mutex>
#include <vector>

#include "nanofs/inode.h"  // inode_id

namespace nanofs {

// Per-inode read/write locks. Pure mechanism: hand out RAII lock guards
// keyed by inode_id. Readers (shared) run concurrently; a writer (exclusive)
// excludes all readers and writers of the *same* inode, while locks on
// different inodes never serialize. The table is fixed-size (num_inodes is
// bounded), so the underlying mutexes never move and guard references stay
// valid for the table's lifetime.
class InodeLockTable {
 public:
  // Pre-build one shared_mutex per inode id (index 0 = nil is unused but kept
  // so an id can index the vector directly).
  explicit InodeLockTable(std::uint32_t num_inodes) : locks_(num_inodes) {}

  // Acquire a shared (reader) lock on `id`; blocks until granted. Multiple
  // readers may hold it at once. Released when the returned guard is destroyed.
  [[nodiscard]] std::shared_lock<std::shared_mutex> lock_shared(inode_id id) {
    return std::shared_lock<std::shared_mutex>(locks_[id]);
  }

  // Acquire an exclusive (writer) lock on `id`; excludes every other holder of
  // the same id. Released when the returned guard is destroyed.
  [[nodiscard]] std::unique_lock<std::shared_mutex> lock_exclusive(inode_id id) {
    return std::unique_lock<std::shared_mutex>(locks_[id]);
  }

 private:
  std::vector<std::shared_mutex> locks_;  // indexed by inode_id
};

}  // namespace nanofs
