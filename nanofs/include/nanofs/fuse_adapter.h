#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "nanofs/error.h"
#include "nanofs/fs.h"
#include "nanofs/fs_backend.h"

namespace nanofs {

struct FuseDirEntry {
  std::string name;
  nanofs::inode_id inode_id;
};

struct FuseMetadata {
  nanofs::inode_id inode_id;
  bool is_dir;
  uint32_t size;
  uint32_t mode;
  uint32_t uid;
  uint32_t gid;
  uint32_t nlink;
};

FuseMetadata fuse_metadata(const Attr& attr, uint32_t uid, uint32_t gid);

class FusePathAdapter {
 public:
  explicit FusePathAdapter(FsBackend& backend) : backend_(backend) {}

  Result<Attr> getattr(const std::string& path);
  Result<std::vector<FuseDirEntry>> readdir(const std::string& path);
  Result<std::string> read(const std::string& path, uint64_t off, uint64_t len);
  Result<uint32_t> write(const std::string& path, uint64_t off, const std::string& data);
  Result<Attr> create(const std::string& path);
  Result<Attr> mkdir(const std::string& path);
  Result<void> unlink(const std::string& path);
  Result<void> rmdir(const std::string& path);
  Result<Attr> truncate(const std::string& path, uint64_t size);

 private:
  Result<FileHandle> resolve(const std::string& path);
  Result<std::pair<FileHandle, std::string>> resolve_parent(const std::string& path);

  FsBackend& backend_;
};

}  // namespace nanofs
