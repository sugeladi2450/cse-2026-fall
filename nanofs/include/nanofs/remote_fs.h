#pragma once
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <rpc/client.h>
#include "nanofs/fs_backend.h"

namespace nanofs {

inline constexpr int kRpcRetries = 3;
inline constexpr int kRpcBackoffMs = 50;
inline constexpr int kRpcTimeoutMs = 1000;

// Wire error code (errno style) -> Error. The inverse of err_code in handlers.cc.
Error code_to_error(int code);

// Bounded retransmission. attempt() sends one RPC and returns Result<T>; a normal return,
// including an in-protocol error, is passed through untouched. An exception (cannot connect,
// reset, timeout) means retransmit with exponential backoff (backoff_ms, 2x each time), at
// most `retries` times, then Error::IoError. Retries can deliver a request more than once; success is not guaranteed.
template <class Fn>
auto call_with_retry(Fn&& attempt, int retries, int backoff_ms) -> std::invoke_result_t<Fn&> {
  for (int i = 0;; ++i) {
    try {
      return attempt();
    } catch (const std::exception&) {
      if (i >= retries) return std::unexpected(Error::IoError);
      std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms << i));
    }
  }
}

// The RPC implementation of FsBackend: each method serialises its arguments into an fs_*
// call, decodes the [err, result] envelope back into Result<T>, and wraps the call in
// call_with_retry. The handle's generation is checked by the remote handler; an unreachable
// server ends as IoError after the retries.
class RemoteFs : public FsBackend {
 public:
  RemoteFs(std::string host, int port, int retries = kRpcRetries,
           int backoff_ms = kRpcBackoffMs, int timeout_ms = kRpcTimeoutMs);

  Result<FileHandle> lookup(FileHandle parent, const std::string& name) override;
  Result<Attr> getattr(FileHandle h) override;
  Result<std::vector<DirEntry>> readdir(FileHandle dir) override;
  Result<std::string> read_file(FileHandle h, uint32_t off, uint32_t len) override;
  Result<uint32_t> write_file(FileHandle h, uint32_t off, const std::string& data) override;
  Result<FileHandle> create(FileHandle parent, const std::string& name) override;
  Result<FileHandle> mkdir(FileHandle parent, const std::string& name) override;
  Result<void> unlink(FileHandle parent, const std::string& name) override;
  Result<void> rmdir(FileHandle parent, const std::string& name) override;
  Result<Attr> setattr(FileHandle h, uint32_t new_size) override;

 private:
  std::unique_ptr<rpc::client> client_;
  int retries_;
  int backoff_ms_;
};

}  // namespace nanofs
