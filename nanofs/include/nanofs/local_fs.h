#pragma once
#include "nanofs/fs_backend.h"
#include "nanofs/fs.h"
#include "nanofs/inode.h"

namespace nanofs {

// Local implementation of FsBackend: take a handle -> check its generation
// with resolve_handle -> delegate to the unmodified FsLayer using the inode_id.
// lookup/create/mkdir pack the resulting inode_id together with its current
// generation into a FileHandle.
class LocalFs : public FsBackend {
 public:
  LocalFs(FsLayer& fs, InodeManager& inodes, bool validate = true)
      : fs_(fs), inodes_(inodes), validate_(validate) {}

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
  // Pack an inode_id together with its current generation into a handle
  // (this is what lookup/create/mkdir return).
  Result<FileHandle> make_handle(inode_id id);
  Result<inode_id> resolve(FileHandle h);
  bool validate_;
  FsLayer& fs_;
  InodeManager& inodes_;
};

}  // namespace nanofs
