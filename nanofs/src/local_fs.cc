#include "nanofs/local_fs.h"

namespace nanofs {

Result<inode_id> LocalFs::resolve(FileHandle h) {
#ifdef NANOFS_STAGE_B
  if (validate_) return resolve_handle(inodes_, h);
#endif
  // Lab1A local adapter uses inode numbers; remote stale-handle checks are Lab1B.
  auto allocated = inodes_.get(h.id);
  if (!allocated) return std::unexpected(allocated.error());
  return h.id;
}


Result<FileHandle> LocalFs::make_handle(inode_id id) {
  auto g = inodes_.generation(id);
  if (!g) return std::unexpected(g.error());
  return FileHandle{id, *g};
}

Result<FileHandle> LocalFs::lookup(FileHandle parent, const std::string& name) {
  auto pid = resolve(parent);
  if (!pid) return std::unexpected(pid.error());
  auto child = fs_.lookup(*pid, name);
  if (!child) return std::unexpected(child.error());
  return make_handle(*child);
}

Result<Attr> LocalFs::getattr(FileHandle h) {
  auto id = resolve(h);
  if (!id) return std::unexpected(id.error());
  return fs_.getattr(*id);
}

Result<std::vector<DirEntry>> LocalFs::readdir(FileHandle dir) {
  auto id = resolve(dir);
  if (!id) return std::unexpected(id.error());
  return fs_.readdir(*id);
}

Result<std::string> LocalFs::read_file(FileHandle h, uint32_t off, uint32_t len) {
  auto id = resolve(h);
  if (!id) return std::unexpected(id.error());
  return fs_.read_file(*id, off, len);
}

Result<uint32_t> LocalFs::write_file(FileHandle h, uint32_t off, const std::string& data) {
  auto id = resolve(h);
  if (!id) return std::unexpected(id.error());
  return fs_.write_file(*id, off, data);
}

Result<FileHandle> LocalFs::create(FileHandle parent, const std::string& name) {
  auto pid = resolve(parent);
  if (!pid) return std::unexpected(pid.error());
  auto id = fs_.create(*pid, name);
  if (!id) return std::unexpected(id.error());
  return make_handle(*id);
}

Result<FileHandle> LocalFs::mkdir(FileHandle parent, const std::string& name) {
  auto pid = resolve(parent);
  if (!pid) return std::unexpected(pid.error());
  auto id = fs_.mkdir(*pid, name);
  if (!id) return std::unexpected(id.error());
  return make_handle(*id);
}

Result<void> LocalFs::unlink(FileHandle parent, const std::string& name) {
  auto pid = resolve(parent);
  if (!pid) return std::unexpected(pid.error());
  return fs_.unlink(*pid, name);
}

Result<void> LocalFs::rmdir(FileHandle parent, const std::string& name) {
  auto pid = resolve(parent);
  if (!pid) return std::unexpected(pid.error());
  return fs_.rmdir(*pid, name);
}

Result<Attr> LocalFs::setattr(FileHandle h, uint32_t new_size) {
  auto id = resolve(h);
  if (!id) return std::unexpected(id.error());
  return fs_.setattr(*id, new_size);
}

}  // namespace nanofs
