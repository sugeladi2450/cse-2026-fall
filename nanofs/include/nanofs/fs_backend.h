#pragma once
#include <string>
#include <vector>
#include "nanofs/error.h"
#include "nanofs/fs.h"       // Attr, DirEntry
#include "nanofs/handle.h"   // FileHandle

namespace nanofs {

// The only storage interface FusePathAdapter talks to: ten handle-addressed
// methods, matching the ten fs_* RPCs one to one. Two implementations: LocalFs
// (in-process) and RemoteFs (over RPC). The adapter cannot tell which one it is
// talking to (access transparency).
class FsBackend {
 public:
  virtual ~FsBackend() = default;
  virtual Result<FileHandle> lookup(FileHandle parent, const std::string& name) = 0;
  virtual Result<Attr> getattr(FileHandle h) = 0;
  virtual Result<std::vector<DirEntry>> readdir(FileHandle dir) = 0;
  virtual Result<std::string> read_file(FileHandle h, uint32_t off, uint32_t len) = 0;
  virtual Result<uint32_t> write_file(FileHandle h, uint32_t off, const std::string& data) = 0;
  virtual Result<FileHandle> create(FileHandle parent, const std::string& name) = 0;
  virtual Result<FileHandle> mkdir(FileHandle parent, const std::string& name) = 0;
  virtual Result<void> unlink(FileHandle parent, const std::string& name) = 0;
  virtual Result<void> rmdir(FileHandle parent, const std::string& name) = 0;
  virtual Result<Attr> setattr(FileHandle h, uint32_t new_size) = 0;
};

}  // namespace nanofs
