#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "nanofs/block.h"

namespace nanofs {

inline constexpr uint32_t NANOFS_JRNL_MAGIC = 0x4A524E4C;  // "JRNL"

// Fixed-size head of a descriptor block. It is immediately followed by
// block_ids[n_records] (a uint32 array) filling out the rest of that one block, so
// one descriptor block describes a whole transaction: which image blocks, how many
// record blocks follow, and the checksum that validates them.
struct JournalDesc {
  uint32_t magic;             // NANOFS_JRNL_MAGIC; anything else => no valid transaction here
  uint32_t n_records;         // number of record blocks in this transaction
  uint32_t records_checksum;  // FNV-1a over the n record blocks; detects a torn tail
};

// Redo write-ahead log as a block-device decorator (Lab2A).
//
// Two devices: `image_` (the disk image — lab 1's backing store, the blocks' final
// "home location") and `journal_` (an independent log device with its own flush
// scope). Between begin() and commit(), metadata writes are buffered in memory in
// this thread's write set; commit() appends them to the journal as one transaction
// and flushes the journal exactly once (the commit point), then applies them to
// the image without flushing. checkpoint() flushes the image and resets the journal.
// File content (write_block with BlockKind::Data) bypasses the log and goes straight
// to the image (writeback), so only metadata is ever redone.
//
// Journal layout: transactions are appended from block 0 as
//   [descriptor @cursor_][record blocks @cursor_+1 .. cursor_+n]
// One transaction fits one descriptor block, so a write set has at most
// (block_size - sizeof(JournalDesc)) / sizeof(block_id) records.
//
// Transaction boundaries are drawn by begin()/commit()/abort(); TxFs wraps each
// mutating fs op in one. Write sets are keyed by thread, so several threads can
// each have a transaction in flight; nesting on one thread is not allowed.
class JournaledBlockDevice : public BlockDevice {
 public:
  JournaledBlockDevice(BlockDevice& image, BlockDevice& journal, block_id checkpoint_threshold)
      : image_(image), journal_(journal), checkpoint_threshold_(checkpoint_threshold) {}

  // Inside a transaction, read this thread's write set first (a multi-step op sees
  // its own intermediate results), otherwise the image.
  Result<void> read_block(block_id id, std::span<std::byte> buf) override;

  // Validate first: `buf` must be one block and `id < image_.num_blocks()` (an
  // out-of-range id buffered unchecked fails only at apply/recover time, after the
  // commit point, when failure can no longer be reported). Then route on `kind`:
  //   Meta — inside a transaction, buffer the bytes in the write set only; the image
  //          is not touched. With no transaction, pass straight through to the image.
  //   Data — bypass the write set and write the image directly (volatile, flushed at
  //          the next checkpoint), and erase any same-id entry from the write set:
  //          allocating a data block called zero_block, which staged a zeroed image
  //          of it; left in place, commit (and a later recover) would overwrite the
  //          content just written with zeros.
  Result<void> write_block(block_id id, std::span<const std::byte> buf,
                           BlockKind kind = BlockKind::Meta) override;

  // Same id validation as write_block. Inside a transaction, put a zeroed block in
  // the write set; otherwise pass through to the image.
  Result<void> zero_block(block_id id) override;

  Result<void> flush() override { return image_.flush(); }
  void reboot() override { image_.reboot(); journal_.reboot(); }
  size_t num_blocks() const override { return image_.num_blocks(); }
  size_t block_size() const override { return image_.block_size(); }

  // Transaction grouping. Not on BlockDevice: only the journaled device has
  // transactions. Each is per calling thread.

  // Open a transaction: an empty write set for this thread. Error::Invalid if this
  // thread is already in one (no nesting).
  Result<void> begin();

  // The WAL commit protocol, under journal_lock_ (one committer at a time):
  //   1. Reject a write set larger than one descriptor can describe (Error::NoSpace)
  //      before touching the log. An empty write set returns at once and consumes
  //      no log block.
  //   2. If `cursor_ + 1 + n > journal_.num_blocks()` (the journal device's size;
  //      num_blocks() on this decorator reports the image's), checkpoint() first to
  //      make room.
  //   3. Append the records at journal[cursor_+1 ..] and the descriptor (magic, n,
  //      checksum, block_ids[]) at journal[cursor_].
  //   4. journal_.flush() — the commit point, the one sequential flush.
  //   5. cursor_ += 1 + n. Apply the write set to the image (volatile, not flushed).
  //   6. If cursor_ >= checkpoint_threshold_, checkpoint().
  // The commit point splits the error semantics:
  // - Before step 4: any failure returns an error and nothing is durable. The
  //   transaction never happened, and the caller may report the op as failed.
  // - Step 4 itself: an error from journal_.flush() is indeterminate. Return it,
  //   but the records may already be on the medium — crash_at_flush(nth, k ==
  //   dirty count) is exactly that — and recover() at the next mount decides: a
  //   complete, checksummed transaction is redone, a torn one is discarded. Under
  //   real power loss no caller survives to see this return value at all.
  // - After step 4 succeeds: the transaction is committed, and commit must not report
  //   failure (the caller would believe it aborted, yet the effect would reappear
  //   after recover()): advance cursor_ first, then log and swallow a device error
  //   in the image apply or the threshold checkpoint — in this model such an error
  //   means fail-stop power loss, and recover() finishes the redo.
  Result<void> commit();

  // Drop this thread's write set. In a redo-only WAL nothing has left memory yet,
  // so there is no undo to perform.
  Result<void> abort();

  // image_.flush() (drain the deferred image writes in one batch) + reset_journal().
  Result<void> checkpoint();

  // Called once at mount, before any transaction. Scan the journal from block 0:
  // a descriptor whose magic is wrong, whose n_records runs past the journal, or
  // whose records fail the checksum ends the scan (a torn tail; everything after
  // it is discarded). Otherwise redo each record block to image block_ids[i]
  // (idempotent: whole-block images). If anything was redone, image_.flush().
  // Finally reset_journal().
  // A device *read error* is not a torn tail. A torn tail is invalid content, which
  // only the crash-truncated, unflushed end of the log can carry; a read error can
  // strike any committed block, the transactions after it may be intact, and the
  // unreadable one may be a redo a later transaction depends on. Propagate the
  // error and fail the mount with the journal untouched (never reset_journal() on
  // this path); a retry or a healthier medium can still recover everything.
  Result<void> recover();

 private:
  // Where blocks finally live: lab 1's disk image, as opposed to the separate
  // journal_ log device.
  BlockDevice& image_;
  BlockDevice& journal_;
  block_id checkpoint_threshold_;
  block_id cursor_ = 0;        // next free block in the journal
  // Write sets are keyed by thread so several transactions can be in flight at once
  // (before-or-after is LockedFs's job with its per-inode locks; the journal only has
  // to keep threads from seeing each other's uncommitted writes). Single-threaded it
  // degenerates to one entry, and the JournalCrash matrix is unaffected.
  using WriteSet = std::map<block_id, std::vector<std::byte>>;
  std::mutex tx_map_mutex_;                              // guards insert/erase on txs_ (not the block data inside)
  std::unordered_map<std::thread::id, WriteSet> txs_;    // this thread's active write set
  std::mutex journal_lock_;                              // guards the log tail: the serial commit critical section (append+flush+apply+cursor)

  // Zero the entire journal region, flush, cursor_ = 0. Shared by checkpoint() and
  // recover(). Zeroing only block 0 leaves the descriptors written before this
  // checkpoint in the log, and a later, shorter run never overwrites that tail — so
  // after a crash recover() walks past its own last transaction into a stale
  // descriptor (valid magic and checksum) and redoes it, silently rolling back
  // committed metadata.
  Result<void> reset_journal();

  // All access to the thread-keyed write-set map goes through these three; the
  // public begin/commit/abort touch txs_ only via them.
  bool create_tx();        // insert an empty write set for this thread; false if it already has one
  WriteSet* current_tx();  // this thread's active write set, or nullptr (pointer stays valid until this thread erases it)
  void erase_tx();         // remove this thread's write set
};

}  // namespace nanofs
