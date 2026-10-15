// RPC handlers (provided): the worked example of the wire contract.
//
// One handler per fs_* RPC method: deserialise the args -> resolve_handle -> call the
// FsLayer operation -> wrap the result with envelope() / envelope_void() into an
// [err, result] reply.
//
// Methods (as in ../proto/fs_rpc.md):
//   fs_lookup / fs_getattr / fs_readdir / fs_read / fs_write /
//   fs_create / fs_mkdir / fs_unlink / fs_rmdir / fs_setattr
//
// The same translation appears in LocalFs (in-process) and in RemoteFs (client-side
// decode). What you write for the network file system is resolve_handle, the generation
// bump and call_with_retry; see docs/lab1-tasks.md §4.

#include "nanofs/handlers.h"
#include "nanofs/handle.h"
#include "nanofs/log.h"
#include <tuple>
#include <string>
#include <vector>

namespace nanofs {
namespace {
// Error -> errno code
int err_code(Error e) {
  switch (e) {
    case Error::NotFound: return 2;   case Error::IoError: return 5;
    case Error::Exists:   return 17;  case Error::NotDir:  return 20;
    case Error::IsDir:    return 21;  case Error::Invalid: return 22;
    case Error::NoSpace:  return 28;  case Error::NotEmpty: return 39;
    case Error::Stale:    return 70;
  }
  return 5;
}
// Encode a Result<T> as [err, T] (T is a default value on failure).
template <class T>
std::tuple<int, T> envelope(const Result<T>& r) {
  if (r) return {0, *r};
  return {err_code(r.error()), T{}};
}
std::tuple<int, bool> envelope_void(const Result<void>& r) {
  if (r) return {0, true};
  return {err_code(r.error()), false};
}
std::tuple<uint32_t,bool,uint32_t> attr_tuple(const Attr& a) { return {a.inode_id, a.is_dir, a.size}; }
std::tuple<uint32_t,uint32_t> handle_tuple(inode_id id, uint32_t gen) { return {id, gen}; }
}  // namespace

void register_fs_handlers(rpc::server& srv, FsLayer& fs, InodeManager& inodes) {
  srv.bind("fs_lookup", [&fs, &inodes](uint32_t pid, uint32_t pgen, std::string n)
           -> std::tuple<int, std::tuple<uint32_t,uint32_t>> {
    auto parent = resolve_handle(inodes, FileHandle{pid, pgen});
    if (!parent) return {err_code(parent.error()), {}};
    auto r = fs.lookup(*parent, n);
    if (!r) { NANOFS_LOG_WARN("fs_lookup parent=" << pid << " name=" << n << " -> err " << to_string(r.error()));
              return {err_code(r.error()), {}}; }
    auto g = inodes.generation(*r);
    if (!g) return {err_code(g.error()), {}};
    NANOFS_LOG_INFO("fs_lookup parent=" << pid << " name=" << n << " -> ok id=" << *r << " gen=" << *g);
    return {0, handle_tuple(*r, *g)};
  });

  srv.bind("fs_getattr", [&fs, &inodes](uint32_t id, uint32_t gen)
           -> std::tuple<int, std::tuple<uint32_t,bool,uint32_t>> {
    auto h = resolve_handle(inodes, FileHandle{id, gen});
    if (!h) return {err_code(h.error()), {}};
    auto r = fs.getattr(*h);
    if (r) { NANOFS_LOG_INFO("fs_getattr id=" << id << " -> ok size=" << r->size);
             return {0, attr_tuple(*r)}; }
    NANOFS_LOG_WARN("fs_getattr id=" << id << " -> err " << to_string(r.error()));
    return {err_code(r.error()), {}};
  });

  srv.bind("fs_readdir", [&fs, &inodes](uint32_t id, uint32_t gen)
           -> std::tuple<int, std::vector<std::tuple<std::string,uint32_t>>> {
    auto h = resolve_handle(inodes, FileHandle{id, gen});
    if (!h) return {err_code(h.error()), {}};
    auto r = fs.readdir(*h);
    if (!r) { NANOFS_LOG_WARN("fs_readdir id=" << id << " -> err " << to_string(r.error()));
              return {err_code(r.error()), {}}; }
    std::vector<std::tuple<std::string,uint32_t>> out;
    for (auto& e : *r) out.emplace_back(std::string(e.name), e.inode_id);
    NANOFS_LOG_INFO("fs_readdir id=" << id << " -> ok n=" << out.size());
    return {0, out};
  });

  srv.bind("fs_read", [&fs, &inodes](uint32_t id, uint32_t gen, uint32_t off, uint32_t len)
           -> std::tuple<int, std::string> {
    auto h = resolve_handle(inodes, FileHandle{id, gen});
    if (!h) return {err_code(h.error()), {}};
    auto r = fs.read_file(*h, off, len);
    if (r) NANOFS_LOG_INFO("fs_read id=" << id << " off=" << off << " len=" << len << " -> ok len=" << r->size());
    else NANOFS_LOG_WARN("fs_read id=" << id << " off=" << off << " len=" << len << " -> err " << to_string(r.error()));
    return envelope(r);
  });

  srv.bind("fs_write", [&fs, &inodes](uint32_t id, uint32_t gen, uint32_t off, std::string data)
           -> std::tuple<int, uint32_t> {
    auto h = resolve_handle(inodes, FileHandle{id, gen});
    if (!h) return {err_code(h.error()), {}};
    auto r = fs.write_file(*h, off, data);
    if (r) NANOFS_LOG_INFO("fs_write id=" << id << " off=" << off << " len=" << data.size() << " -> ok written=" << *r);
    else NANOFS_LOG_WARN("fs_write id=" << id << " off=" << off << " len=" << data.size() << " -> err " << to_string(r.error()));
    return envelope(r);
  });

  srv.bind("fs_create", [&fs, &inodes](uint32_t pid, uint32_t pgen, std::string n)
           -> std::tuple<int, std::tuple<uint32_t,uint32_t>> {
    auto parent = resolve_handle(inodes, FileHandle{pid, pgen});
    if (!parent) return {err_code(parent.error()), {}};
    auto r = fs.create(*parent, n);
    if (!r) { NANOFS_LOG_WARN("fs_create parent=" << pid << " name=" << n << " -> err " << to_string(r.error()));
              return {err_code(r.error()), {}}; }
    auto g = inodes.generation(*r);
    if (!g) return {err_code(g.error()), {}};
    NANOFS_LOG_INFO("fs_create parent=" << pid << " name=" << n << " -> ok id=" << *r << " gen=" << *g);
    return {0, handle_tuple(*r, *g)};
  });

  srv.bind("fs_mkdir", [&fs, &inodes](uint32_t pid, uint32_t pgen, std::string n)
           -> std::tuple<int, std::tuple<uint32_t,uint32_t>> {
    auto parent = resolve_handle(inodes, FileHandle{pid, pgen});
    if (!parent) return {err_code(parent.error()), {}};
    auto r = fs.mkdir(*parent, n);
    if (!r) { NANOFS_LOG_WARN("fs_mkdir parent=" << pid << " name=" << n << " -> err " << to_string(r.error()));
              return {err_code(r.error()), {}}; }
    auto g = inodes.generation(*r);
    if (!g) return {err_code(g.error()), {}};
    NANOFS_LOG_INFO("fs_mkdir parent=" << pid << " name=" << n << " -> ok id=" << *r << " gen=" << *g);
    return {0, handle_tuple(*r, *g)};
  });

  srv.bind("fs_unlink", [&fs, &inodes](uint32_t pid, uint32_t pgen, std::string n)
           -> std::tuple<int, bool> {
    auto parent = resolve_handle(inodes, FileHandle{pid, pgen});
    if (!parent) return {err_code(parent.error()), false};
    auto r = fs.unlink(*parent, n);
    if (r) NANOFS_LOG_INFO("fs_unlink parent=" << pid << " name=" << n << " -> ok");
    else NANOFS_LOG_WARN("fs_unlink parent=" << pid << " name=" << n << " -> err " << to_string(r.error()));
    return envelope_void(r);
  });

  srv.bind("fs_rmdir", [&fs, &inodes](uint32_t pid, uint32_t pgen, std::string n)
           -> std::tuple<int, bool> {
    auto parent = resolve_handle(inodes, FileHandle{pid, pgen});
    if (!parent) return {err_code(parent.error()), false};
    auto r = fs.rmdir(*parent, n);
    if (r) NANOFS_LOG_INFO("fs_rmdir parent=" << pid << " name=" << n << " -> ok");
    else NANOFS_LOG_WARN("fs_rmdir parent=" << pid << " name=" << n << " -> err " << to_string(r.error()));
    return envelope_void(r);
  });

  srv.bind("fs_setattr", [&fs, &inodes](uint32_t id, uint32_t gen, uint32_t sz)
           -> std::tuple<int, std::tuple<uint32_t,bool,uint32_t>> {
    auto h = resolve_handle(inodes, FileHandle{id, gen});
    if (!h) return {err_code(h.error()), {}};
    auto r = fs.setattr(*h, sz);
    if (r) { NANOFS_LOG_INFO("fs_setattr id=" << id << " size=" << sz << " -> ok");
             return {0, attr_tuple(*r)}; }
    NANOFS_LOG_WARN("fs_setattr id=" << id << " size=" << sz << " -> err " << to_string(r.error()));
    return {err_code(r.error()), {}};
  });
}
}  // namespace nanofs
