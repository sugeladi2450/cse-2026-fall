#include "nanofs/remote_fs.h"
#include <cstring>

namespace nanofs {

Error code_to_error(int code) {
  switch (code) {
    case 2:  return Error::NotFound;   case 5:  return Error::IoError;
    case 17: return Error::Exists;     case 20: return Error::NotDir;
    case 21: return Error::IsDir;      case 22: return Error::Invalid;
    case 28: return Error::NoSpace;    case 39: return Error::NotEmpty;
    case 70: return Error::Stale;
  }
  return Error::IoError;
}

RemoteFs::RemoteFs(std::string host, int port, int retries, int backoff_ms, int timeout_ms)
    : client_(std::make_unique<rpc::client>(host, static_cast<uint16_t>(port))),
      retries_(retries), backoff_ms_(backoff_ms) {
  client_->set_timeout(timeout_ms);
}

namespace {
// Decode [err, [id, gen]] into a Result<FileHandle>.
Result<FileHandle> to_handle(const std::tuple<int, std::tuple<uint32_t,uint32_t>>& env) {
  if (std::get<0>(env) != 0) return std::unexpected(code_to_error(std::get<0>(env)));
  auto [id, gen] = std::get<1>(env);
  return FileHandle{id, gen};
}
// Decode [err, [id, is_dir, size]] into a Result<Attr>.
Result<Attr> to_attr(const std::tuple<int, std::tuple<uint32_t,bool,uint32_t>>& env) {
  if (std::get<0>(env) != 0) return std::unexpected(code_to_error(std::get<0>(env)));
  auto [id, is_dir, size] = std::get<1>(env);
  return Attr{id, is_dir, size};
}
}  // namespace

Result<FileHandle> RemoteFs::lookup(FileHandle parent, const std::string& name) {
  return call_with_retry([&]() -> Result<FileHandle> {
    return to_handle(client_->call("fs_lookup", parent.id, parent.generation, name)
                       .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>());
  }, retries_, backoff_ms_);
}

Result<Attr> RemoteFs::getattr(FileHandle h) {
  return call_with_retry([&]() -> Result<Attr> {
    return to_attr(client_->call("fs_getattr", h.id, h.generation)
                     .as<std::tuple<int, std::tuple<uint32_t,bool,uint32_t>>>());
  }, retries_, backoff_ms_);
}

Result<std::vector<DirEntry>> RemoteFs::readdir(FileHandle dir) {
  return call_with_retry([&]() -> Result<std::vector<DirEntry>> {
    auto [err, rows] = client_->call("fs_readdir", dir.id, dir.generation)
        .as<std::tuple<int, std::vector<std::tuple<std::string,uint32_t>>>>();
    if (err != 0) return std::unexpected(code_to_error(err));
    std::vector<DirEntry> out;
    out.reserve(rows.size());
    for (auto& [name, id] : rows) {
      DirEntry e{}; e.inode_id = id;
      std::strncpy(e.name, name.c_str(), NANOFS_NAME_MAX - 1);
      out.push_back(e);
    }
    return out;
  }, retries_, backoff_ms_);
}

Result<std::string> RemoteFs::read_file(FileHandle h, uint32_t off, uint32_t len) {
  return call_with_retry([&]() -> Result<std::string> {
    auto [err, data] = client_->call("fs_read", h.id, h.generation, off, len)
                         .as<std::tuple<int, std::string>>();
    if (err != 0) return std::unexpected(code_to_error(err));
    return data;
  }, retries_, backoff_ms_);
}

Result<uint32_t> RemoteFs::write_file(FileHandle h, uint32_t off, const std::string& data) {
  return call_with_retry([&]() -> Result<uint32_t> {
    auto [err, n] = client_->call("fs_write", h.id, h.generation, off, data)
                      .as<std::tuple<int, uint32_t>>();
    if (err != 0) return std::unexpected(code_to_error(err));
    return n;
  }, retries_, backoff_ms_);
}

Result<FileHandle> RemoteFs::create(FileHandle parent, const std::string& name) {
  return call_with_retry([&]() -> Result<FileHandle> {
    return to_handle(client_->call("fs_create", parent.id, parent.generation, name)
                       .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>());
  }, retries_, backoff_ms_);
}

Result<FileHandle> RemoteFs::mkdir(FileHandle parent, const std::string& name) {
  return call_with_retry([&]() -> Result<FileHandle> {
    return to_handle(client_->call("fs_mkdir", parent.id, parent.generation, name)
                       .as<std::tuple<int, std::tuple<uint32_t,uint32_t>>>());
  }, retries_, backoff_ms_);
}

Result<void> RemoteFs::unlink(FileHandle parent, const std::string& name) {
  return call_with_retry([&]() -> Result<void> {
    auto [err, ok] = client_->call("fs_unlink", parent.id, parent.generation, name)
                       .as<std::tuple<int, bool>>();
    if (err != 0) return std::unexpected(code_to_error(err));
    return {};
  }, retries_, backoff_ms_);
}

Result<void> RemoteFs::rmdir(FileHandle parent, const std::string& name) {
  return call_with_retry([&]() -> Result<void> {
    auto [err, ok] = client_->call("fs_rmdir", parent.id, parent.generation, name)
                       .as<std::tuple<int, bool>>();
    if (err != 0) return std::unexpected(code_to_error(err));
    return {};
  }, retries_, backoff_ms_);
}

Result<Attr> RemoteFs::setattr(FileHandle h, uint32_t new_size) {
  return call_with_retry([&]() -> Result<Attr> {
    return to_attr(client_->call("fs_setattr", h.id, h.generation, new_size)
                     .as<std::tuple<int, std::tuple<uint32_t,bool,uint32_t>>>());
  }, retries_, backoff_ms_);
}

}  // namespace nanofs
