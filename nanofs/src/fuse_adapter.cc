#include "nanofs/fuse_adapter.h"
#include <limits>
#include <string_view>
#include <sys/stat.h>

namespace nanofs {
namespace {
Result<std::vector<std::string>> split_path(const std::string& path) {
  if (path.empty() || path[0] != '/') return std::unexpected(Error::Invalid);
  std::vector<std::string> parts;
  size_t pos = 1;
  while (pos < path.size()) {
    while (pos < path.size() && path[pos] == '/') ++pos;
    const size_t start = pos;
    while (pos < path.size() && path[pos] != '/') ++pos;
    if (start == pos) break;
    std::string part = path.substr(start, pos - start);
    if (part == "." || part == "..") return std::unexpected(Error::Invalid);
    parts.push_back(std::move(part));
  }
  return parts;
}

bool fits_u32(uint64_t n) {
  return n <= std::numeric_limits<uint32_t>::max();
}
}  // namespace

FuseMetadata fuse_metadata(const Attr& attr, uint32_t uid, uint32_t gid) {
  return FuseMetadata{
    attr.inode_id,
    attr.is_dir,
    attr.size,
    static_cast<uint32_t>(attr.is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644)),
    uid,
    gid,
    static_cast<uint32_t>(attr.is_dir ? 2 : 1),
  };
}

Result<FileHandle> FusePathAdapter::resolve(const std::string& path) {
  auto parts = split_path(path);
  if (!parts) return std::unexpected(parts.error());
  FileHandle cur{ROOT_INODE, ROOT_GENERATION};
  for (const auto& part : *parts) {
    auto next = backend_.lookup(cur, part);
    if (!next) return std::unexpected(next.error());
    cur = *next;
  }
  return cur;
}

Result<std::pair<FileHandle, std::string>> FusePathAdapter::resolve_parent(const std::string& path) {
  auto parts = split_path(path);
  if (!parts) return std::unexpected(parts.error());
  if (parts->empty()) return std::unexpected(Error::Invalid);
  std::string name = parts->back();
  parts->pop_back();
  FileHandle parent{ROOT_INODE, ROOT_GENERATION};
  for (const auto& part : *parts) {
    auto next = backend_.lookup(parent, part);
    if (!next) return std::unexpected(next.error());
    parent = *next;
  }
  return std::pair<FileHandle, std::string>{parent, name};
}

Result<Attr> FusePathAdapter::getattr(const std::string& path) {
  auto h = resolve(path);
  if (!h) return std::unexpected(h.error());
  return backend_.getattr(*h);
}

Result<std::vector<FuseDirEntry>> FusePathAdapter::readdir(const std::string& path) {
  auto h = resolve(path);
  if (!h) return std::unexpected(h.error());
  auto entries = backend_.readdir(*h);
  if (!entries) return std::unexpected(entries.error());
  std::vector<FuseDirEntry> out;
  out.reserve(entries->size());
  for (const auto& entry : *entries) out.push_back({entry.name, entry.inode_id});
  return out;
}

Result<std::string> FusePathAdapter::read(const std::string& path, uint64_t off, uint64_t len) {
  if (!fits_u32(off) || !fits_u32(len) || off + len > std::numeric_limits<uint32_t>::max())
    return std::unexpected(Error::Invalid);
  auto h = resolve(path);
  if (!h) return std::unexpected(h.error());
  return backend_.read_file(*h, static_cast<uint32_t>(off), static_cast<uint32_t>(len));
}

Result<uint32_t> FusePathAdapter::write(const std::string& path, uint64_t off, const std::string& data) {
  if (!fits_u32(off) || data.size() > std::numeric_limits<uint32_t>::max() ||
      off + data.size() > std::numeric_limits<uint32_t>::max())
    return std::unexpected(Error::Invalid);
  auto h = resolve(path);
  if (!h) return std::unexpected(h.error());
  return backend_.write_file(*h, static_cast<uint32_t>(off), data);
}

Result<Attr> FusePathAdapter::create(const std::string& path) {
  auto parent = resolve_parent(path);
  if (!parent) return std::unexpected(parent.error());
  auto h = backend_.create(parent->first, parent->second);
  if (!h) return std::unexpected(h.error());
  return backend_.getattr(*h);
}

Result<Attr> FusePathAdapter::mkdir(const std::string& path) {
  auto parent = resolve_parent(path);
  if (!parent) return std::unexpected(parent.error());
  auto h = backend_.mkdir(parent->first, parent->second);
  if (!h) return std::unexpected(h.error());
  return backend_.getattr(*h);
}

Result<void> FusePathAdapter::unlink(const std::string& path) {
  auto parent = resolve_parent(path);
  if (!parent) return std::unexpected(parent.error());
  return backend_.unlink(parent->first, parent->second);
}

Result<void> FusePathAdapter::rmdir(const std::string& path) {
  auto parent = resolve_parent(path);
  if (!parent) return std::unexpected(parent.error());
  return backend_.rmdir(parent->first, parent->second);
}

Result<Attr> FusePathAdapter::truncate(const std::string& path, uint64_t size) {
  if (!fits_u32(size)) return std::unexpected(Error::Invalid);
  auto h = resolve(path);
  if (!h) return std::unexpected(h.error());
  return backend_.setattr(*h, static_cast<uint32_t>(size));
}

}  // namespace nanofs
