#include "nanofs/txfs.h"

namespace nanofs {

namespace {
// Run fn inside one journal transaction: begin -> fn() -> commit on success / abort
// on failure. fn returns Result<T> (Result<void> included); a failing begin/commit
// is also turned into an error result.
template <class Fn>
auto in_tx(JournaledBlockDevice& j, Fn&& fn) -> decltype(fn()) {
  if (auto r = j.begin(); !r) return std::unexpected(r.error());
  auto out = fn();
  if (!out) { (void)j.abort(); return out; }
  if (auto r = j.commit(); !r) return std::unexpected(r.error());
  return out;
}
}  // namespace

Result<uint32_t> TxFs::write_file(inode_id id, uint32_t off, const std::string& data) {
  return in_tx(jdev_, [&] { return FsLayer::write_file(id, off, data); });
}

Result<void> TxFs::unlink(inode_id parent, const std::string& name) {
  return in_tx(jdev_, [&] { return FsLayer::unlink(parent, name); });
}

Result<void> TxFs::rmdir(inode_id parent, const std::string& name) {
  return in_tx(jdev_, [&] { return FsLayer::rmdir(parent, name); });
}

Result<Attr> TxFs::setattr(inode_id id, uint32_t new_size) {
  return in_tx(jdev_, [&] { return FsLayer::setattr(id, new_size); });
}

Result<inode_id> TxFs::make_node(inode_id parent, const std::string& name, uint32_t type) {
  return in_tx(jdev_, [&] { return FsLayer::make_node(parent, name, type); });
}

}  // namespace nanofs
