// concurrent_bench — the lab 2 bonus benchmark for fine-grained locking.
//
// One global lock already gives before-or-after atomicity and passes every
// correctness test; all it loses is concurrency. This benchmark runs the same
// read-heavy workload on two file systems, one after the other:
//
//   GlobalLockFs  — one global mutex per op (the deliberately coarse baseline, defined here)
//   LockedFs      — your per-inode read/write locks + lazy allocator lock
//
// Both run on separate, identically formatted device stacks with the same thread
// count and the same op sequence; only the lock policy differs, so the ratio of the
// two wall-clock times is the ratio of their concurrency. Prints one line
// `speedup <x> ...`; reaching the threshold earns the bonus.
//
// The workload is "every thread reads its own file + a few in-place overwrites":
// nothing allocates, so the allocator lock never engages, and reads take only shared
// locks, so the fine-grained version truly runs in parallel while the global-lock
// version serializes.
//
//   cmake --build build --target run_concurrent_stress
#include <nanofs/block.h>
#include <nanofs/inode.h>
#include <nanofs/journal.h>
#include <nanofs/locked_fs.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace nanofs;

namespace {

constexpr uint32_t kInodes = 128;
constexpr size_t kBlockSize = 4096, kNumBlocks = 4096;
constexpr int kFileBlocks = 4;         // size of each thread's own file
constexpr int kReadsPerWrite = 99;     // read-heavy: 99% reads
constexpr double kBonusSpeedup = 2.0;  // bonus bar: half the global lock's time

// Baseline: one lock around every op, held through commit. Correct, with no
// concurrency at all.
class GlobalLockFs : public TxFs {
 public:
  using TxFs::TxFs;

  Result<inode_id> lookup(inode_id parent, const std::string& name) override {
    std::lock_guard<std::mutex> g(m_);
    return FsLayer::lookup(parent, name);
  }
  Result<Attr> getattr(inode_id id) override {
    std::lock_guard<std::mutex> g(m_);
    return FsLayer::getattr(id);
  }
  Result<std::vector<DirEntry>> readdir(inode_id dir) override {
    std::lock_guard<std::mutex> g(m_);
    return FsLayer::readdir(dir);
  }
  Result<std::string> read_file(inode_id id, uint32_t off, uint32_t len) override {
    std::lock_guard<std::mutex> g(m_);
    return FsLayer::read_file(id, off, len);
  }
  Result<uint32_t> write_file(inode_id id, uint32_t off, const std::string& data) override {
    std::lock_guard<std::mutex> g(m_);
    return TxFs::write_file(id, off, data);
  }
  Result<Attr> setattr(inode_id id, uint32_t new_size) override {
    std::lock_guard<std::mutex> g(m_);
    return TxFs::setattr(id, new_size);
  }
  Result<void> unlink(inode_id parent, const std::string& name) override {
    std::lock_guard<std::mutex> g(m_);
    return TxFs::unlink(parent, name);
  }
  Result<void> rmdir(inode_id parent, const std::string& name) override {
    std::lock_guard<std::mutex> g(m_);
    return TxFs::rmdir(parent, name);
  }

 protected:
  Result<inode_id> make_node(inode_id parent, const std::string& name, uint32_t type) override {
    std::lock_guard<std::mutex> g(m_);
    return TxFs::make_node(parent, name, type);
  }

 private:
  std::mutex m_;
};

// One formatted device stack; the caller picks the fs by lock policy.
struct Stack {
  SimulatedBlockDevice image{kBlockSize, kNumBlocks}, jbk{kBlockSize, kNumBlocks};
  JournaledBlockDevice j{image, jbk, /*threshold=*/kNumBlocks * 3 / 4};
  InodeManager im{j, kInodes};
  Stack() {
    if (!im.format() || !j.checkpoint()) { std::fputs("format failed\n", stderr); std::exit(2); }
  }
};

// One file per thread, pre-filled with kFileBlocks blocks; returns the inode ids.
std::vector<inode_id> seed_files(FsLayer& fs, int threads) {
  std::vector<inode_id> files;
  const std::string block(kBlockSize, 'x');
  for (int i = 0; i < threads; ++i) {
    auto f = fs.create(ROOT_INODE, "f" + std::to_string(i));
    if (!f) { std::fputs("seed create failed\n", stderr); std::exit(2); }
    for (int b = 0; b < kFileBlocks; ++b)
      if (!fs.write_file(*f, b * kBlockSize, block)) { std::fputs("seed write failed\n", stderr); std::exit(2); }
    files.push_back(*f);
  }
  return files;
}

// Run the workload, return wall-clock ms. Each thread does iters rounds of 99 reads + 1 in-place write, all on its own file.
long long run_workload(FsLayer& fs, int threads, int iters) {
  auto files = seed_files(fs, threads);
  const std::string payload(256, 'w');
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> ts;
  for (int i = 0; i < threads; ++i) {
    ts.emplace_back([&, i] {
      for (int k = 0; k < iters; ++k) {
        for (int r = 0; r < kReadsPerWrite; ++r) {
          const uint32_t off = static_cast<uint32_t>((k + r) % kFileBlocks) * kBlockSize;
          (void)fs.read_file(files[i], off, kBlockSize);
        }
        (void)fs.write_file(files[i], (k % kFileBlocks) * kBlockSize, payload);  // in-place overwrite: allocates nothing
      }
    });
  }
  for (auto& th : ts) th.join();
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  const int threads = argc > 1 ? std::atoi(argv[1])
                               : static_cast<int>(std::min(8u, std::max(2u, std::thread::hardware_concurrency())));
  const int iters = argc > 2 ? std::atoi(argv[2]) : 800;
  const long long ops = 1LL * threads * iters * (kReadsPerWrite + 1);

  long long global_ms = 0, locked_ms = 0;
  { Stack s; GlobalLockFs fs(s.im, s.j); global_ms = run_workload(fs, threads, iters); }
  { Stack s; LockedFs fs(s.im, s.j, kInodes); locked_ms = run_workload(fs, threads, iters); }

  const double speedup = locked_ms > 0 ? static_cast<double>(global_ms) / locked_ms : 0.0;
  std::printf("threads %d  iters %d  ops %lld  mix %d%%read\n", threads, iters, ops,
              kReadsPerWrite * 100 / (kReadsPerWrite + 1));
  std::printf("global-lock %lld ms\n", global_ms);
  std::printf("locked-fs   %lld ms\n", locked_ms);
  std::printf("speedup %.2fx  threshold %.2fx  %s\n", speedup, kBonusSpeedup,
              speedup >= kBonusSpeedup ? "PASS" : "FAIL");
  return speedup >= kBonusSpeedup ? 0 : 1;
}
