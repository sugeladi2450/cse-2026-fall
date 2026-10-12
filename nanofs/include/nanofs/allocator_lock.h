#pragma once
#include <mutex>

namespace nanofs {

// Lazy lock that serializes allocation to prevent the whole-block-redo clobber
// on shared metadata blocks (block/inode bitmaps, inode table). Only ops that
// actually allocate/free pay for it, and they pay only from the first
// allocation through commit.
//
// Usage:
//   - LockedFs opens an AllocatorLock::Scope for the duration of a mutating op.
//   - The allocator (BlockAllocator / InodeManager) calls
//     AllocatorLock::engage_current() on entry to allocate/free. The first such
//     call locks the lock's own mutex for this thread's scope; later calls are
//     no-ops. The lock is held until the Scope is destroyed (after commit), so
//     the mutated shared block cannot be clobbered by a concurrent allocator.
//   - An op that never allocates never engages, so it never takes the mutex and
//     never blocks a concurrent op.
//
// Single writer per op: a thread has at most one active Scope (fs ops do not
// nest), tracked in a thread-local pointer so the allocator can reach it
// without threading a reference through every call.
class AllocatorLock {
 public:
  AllocatorLock() = default;
  AllocatorLock(const AllocatorLock&) = delete;
  AllocatorLock& operator=(const AllocatorLock&) = delete;

  // Per-op RAII scope. Engaging is deferred to the first engage_current().
  class Scope {
   public:
    explicit Scope(AllocatorLock& lock) : lock_(lock), prev_(current_) {
      current_ = this;
    }
    ~Scope() {
      if (engaged_) lock_.mutex_.unlock();
      current_ = prev_;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    friend class AllocatorLock;
    // Acquire the lock once for this scope; idempotent within the scope.
    void engage() {
      if (!engaged_) {
        lock_.mutex_.lock();
        engaged_ = true;
      }
    }
    AllocatorLock& lock_;
    Scope* prev_;          // restored on destruction (defensive; ops don't nest)
    bool engaged_ = false;
  };

  // Engage the lock for the calling thread's active Scope, if any. Called by
  // the allocator; idempotent per scope; a no-op when no Scope is active.
  static void engage_current() {
    if (current_) current_->engage();
  }

 private:
  std::mutex mutex_;
  inline static thread_local Scope* current_ = nullptr;
};

}  // namespace nanofs
