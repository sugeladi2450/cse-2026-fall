#include <nanofs/block.h>
#include <nanofs/fuse_adapter.h>
#include <nanofs/fs.h>
#include <nanofs/local_fs.h>
#ifdef NANOFS_STAGE_B
#include <nanofs/remote_fs.h>
#endif
#include <nanofs/fs_backend.h>
#include <nanofs/inode.h>
#include <nanofs/log.h>
#include <fuse.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace nanofs;

namespace {
struct App {
  std::unique_ptr<SimulatedBlockDevice> dev;
  std::unique_ptr<InodeManager> inodes;
  std::unique_ptr<FsLayer> fs;                  // local mode only
  std::unique_ptr<FsBackend> backend;           // LocalFs or RemoteFs
  std::unique_ptr<FusePathAdapter> adapter;
  uid_t uid = 0;
  gid_t gid = 0;
};

App* app() {
  return static_cast<App*>(fuse_get_context()->private_data);
}

int to_errno(Error e) {
  switch (e) {
    case Error::NotFound: return -ENOENT;
    case Error::IoError: return -EIO;
    case Error::Exists: return -EEXIST;
    case Error::NotDir: return -ENOTDIR;
    case Error::IsDir: return -EISDIR;
    case Error::Invalid: return -EINVAL;
    case Error::NoSpace: return -ENOSPC;
    case Error::NotEmpty: return -ENOTEMPTY;
    case Error::Stale: return -ESTALE;
  }
  return -EIO;
}

void fill_stat(const Attr& attr, struct stat* st) {
  auto meta = fuse_metadata(attr, static_cast<uint32_t>(app()->uid), static_cast<uint32_t>(app()->gid));
  std::memset(st, 0, sizeof(*st));
  st->st_ino = meta.inode_id;
  st->st_mode = meta.mode;
  st->st_nlink = meta.nlink;
  st->st_size = meta.size;
  st->st_uid = meta.uid;
  st->st_gid = meta.gid;
}

#ifdef NANOFS_USE_FUSE3
int op_getattr(const char* path, struct stat* st, struct fuse_file_info*) {
#else
int op_getattr(const char* path, struct stat* st) {
#endif
  auto r = app()->adapter->getattr(path);
  if (!r) {
    NANOFS_LOG_WARN("op_getattr path=" << path << " -> err " << to_string(r.error()));
    return to_errno(r.error());
  }
  NANOFS_LOG_INFO("op_getattr path=" << path << " -> ok");
  fill_stat(*r, st);
  return 0;
}

int op_mkdir(const char* path, mode_t) {
  auto r = app()->adapter->mkdir(path);
  if (!r) {
    NANOFS_LOG_WARN("op_mkdir path=" << path << " -> err " << to_string(r.error()));
    return to_errno(r.error());
  }
  NANOFS_LOG_INFO("op_mkdir path=" << path << " -> ok");
  return 0;
}

int op_create(const char* path, mode_t, struct fuse_file_info*) {
  auto r = app()->adapter->create(path);
  if (!r) {
    NANOFS_LOG_WARN("op_create path=" << path << " -> err " << to_string(r.error()));
    return to_errno(r.error());
  }
  NANOFS_LOG_INFO("op_create path=" << path << " -> ok");
  return 0;
}

int op_open(const char* path, struct fuse_file_info*) {
  auto r = app()->adapter->getattr(path);
  if (!r) {
    NANOFS_LOG_WARN("op_open path=" << path << " -> err " << to_string(r.error()));
    return to_errno(r.error());
  }
  if (r->is_dir) {
    NANOFS_LOG_WARN("op_open path=" << path << " -> err " << to_string(Error::IsDir));
    return -EISDIR;
  }
  NANOFS_LOG_INFO("op_open path=" << path << " -> ok");
  return 0;
}

int op_unlink(const char* path) {
  auto r = app()->adapter->unlink(path);
  if (!r) {
    NANOFS_LOG_WARN("op_unlink path=" << path << " -> err " << to_string(r.error()));
    return to_errno(r.error());
  }
  NANOFS_LOG_INFO("op_unlink path=" << path << " -> ok");
  return 0;
}

int op_rmdir(const char* path) {
  auto r = app()->adapter->rmdir(path);
  if (!r) {
    NANOFS_LOG_WARN("op_rmdir path=" << path << " -> err " << to_string(r.error()));
    return to_errno(r.error());
  }
  NANOFS_LOG_INFO("op_rmdir path=" << path << " -> ok");
  return 0;
}

#ifdef NANOFS_USE_FUSE3
int op_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info*,
               enum fuse_readdir_flags) {
  filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
  filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
#else
int op_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info*) {
  filler(buf, ".", nullptr, 0);
  filler(buf, "..", nullptr, 0);
#endif
  auto entries = app()->adapter->readdir(path);
  if (!entries) {
    NANOFS_LOG_WARN("op_readdir path=" << path << " -> err " << to_string(entries.error()));
    return to_errno(entries.error());
  }
  for (const auto& entry : *entries) {
    struct stat st{};
    st.st_ino = entry.inode_id;
#ifdef NANOFS_USE_FUSE3
    filler(buf, entry.name.c_str(), &st, 0, static_cast<fuse_fill_dir_flags>(0));
#else
    filler(buf, entry.name.c_str(), &st, 0);
#endif
  }
  NANOFS_LOG_INFO("op_readdir path=" << path << " -> ok n=" << entries->size());
  return 0;
}

int op_read(const char* path, char* buf, size_t size, off_t off, struct fuse_file_info*) {
  if (off < 0) return -EINVAL;
  auto r = app()->adapter->read(path, static_cast<uint64_t>(off), size);
  if (!r) {
    NANOFS_LOG_WARN("op_read path=" << path << " off=" << off << " len=" << size << " -> err " << to_string(r.error()));
    return to_errno(r.error());
  }
  std::memcpy(buf, r->data(), r->size());
  NANOFS_LOG_INFO("op_read path=" << path << " off=" << off << " len=" << size << " -> ok len=" << r->size());
  return static_cast<int>(r->size());
}

int op_write(const char* path, const char* buf, size_t size, off_t off, struct fuse_file_info*) {
  if (off < 0) return -EINVAL;
  auto r = app()->adapter->write(path, static_cast<uint64_t>(off), std::string(buf, size));
  if (!r) {
    NANOFS_LOG_WARN("op_write path=" << path << " off=" << off << " len=" << size << " -> err " << to_string(r.error()));
    return to_errno(r.error());
  }
  NANOFS_LOG_INFO("op_write path=" << path << " off=" << off << " len=" << size << " -> ok written=" << *r);
  return static_cast<int>(*r);
}

#ifdef NANOFS_USE_FUSE3
int op_truncate(const char* path, off_t size, struct fuse_file_info*) {
#else
int op_truncate(const char* path, off_t size) {
#endif
  if (size < 0) return -EINVAL;
  auto r = app()->adapter->truncate(path, static_cast<uint64_t>(size));
  if (!r) {
    NANOFS_LOG_WARN("op_truncate path=" << path << " size=" << size << " -> err " << to_string(r.error()));
    return to_errno(r.error());
  }
  NANOFS_LOG_INFO("op_truncate path=" << path << " size=" << size << " -> ok");
  return 0;
}

fuse_operations operations() {
  fuse_operations ops{};
  ops.getattr = op_getattr;
  ops.mkdir = op_mkdir;
  ops.create = op_create;
  ops.open = op_open;
  ops.unlink = op_unlink;
  ops.rmdir = op_rmdir;
  ops.readdir = op_readdir;
  ops.read = op_read;
  ops.write = op_write;
  ops.truncate = op_truncate;
  return ops;
}

void usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [--server host:port] [--image path] [--format] [--block-size n] "
               "[--num-blocks n] [--num-inodes n] [--log-level debug|info|warn|error] "
               "[--log-file path] <mountpoint> [FUSE options]\n",
               prog);
}
}  // namespace

int main(int argc, char** argv) {
  uint32_t block_size = 4096, num_blocks = 4096, num_inodes = 1024;
  std::string image, log_file, log_level_str = "info", server;
  bool do_format = false;
  std::vector<char*> fuse_argv;
  fuse_argv.push_back(argv[0]);

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (a == "--server") {
      char* value = next();
      if (!value) { usage(argv[0]); return 2; }
      server = value;
    } else if (a == "--image") {
      char* value = next();
      if (!value) { usage(argv[0]); return 2; }
      image = value;
    } else if (a == "--format") {
      do_format = true;
    } else if (a == "--block-size") {
      char* value = next();
      if (!value) { usage(argv[0]); return 2; }
      block_size = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (a == "--num-blocks") {
      char* value = next();
      if (!value) { usage(argv[0]); return 2; }
      num_blocks = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (a == "--num-inodes") {
      char* value = next();
      if (!value) { usage(argv[0]); return 2; }
      num_inodes = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (a == "--log-level") {
      char* value = next();
      if (!value) { usage(argv[0]); return 2; }
      log_level_str = value;
    } else if (a == "--log-file") {
      char* value = next();
      if (!value) { usage(argv[0]); return 2; }
      log_file = value;
    } else {
      fuse_argv.push_back(argv[i]);
    }
  }

  auto level = log::parse_level(log_level_str);
  if (!level) { std::fprintf(stderr, "bad --log-level '%s' (want debug|info|warn|error)\n", log_level_str.c_str()); return 2; }
  if (auto r = log::init(*level, log_file); !r) {
    std::fprintf(stderr, "failed to open --log-file '%s'\n", log_file.c_str()); return 1;
  }

  App state;
  state.uid = getuid();
  state.gid = getgid();
  if (!server.empty()) {
#ifdef NANOFS_STAGE_B
    // Remote mode: forward every file operation to the nanofsd at host:port.
    auto colon = server.rfind(':');
    if (colon == std::string::npos) {
      std::fprintf(stderr, "bad --server '%s' (want host:port)\n", server.c_str()); return 2;
    }
    std::string host = server.substr(0, colon);
    int port = std::atoi(server.c_str() + colon + 1);
    state.backend = std::make_unique<RemoteFs>(host, port);
    NANOFS_LOG_INFO("nanofs_fuse remote backend " << host << ":" << port);
 #else
    std::fprintf(stderr, "Remote mode is released in Lab1B\n"); return 2;
 #endif
  } else {
    // Local mode: in-process block device + inode layer + FsLayer.
    if (!image.empty()) {
      auto dev = SimulatedBlockDevice::open_file(image, block_size, num_blocks);
      if (!dev) { NANOFS_LOG_ERROR("open image failed: " << image); return 1; }
      state.dev = std::move(*dev);
    } else {
      state.dev = std::make_unique<SimulatedBlockDevice>(block_size, num_blocks);
      do_format = true;
    }
    state.inodes = std::make_unique<InodeManager>(*state.dev, num_inodes);
    if (do_format && !state.inodes->format()) { NANOFS_LOG_ERROR("format failed"); return 1; }
    state.fs = std::make_unique<FsLayer>(*state.inodes, *state.dev);
    state.backend = std::make_unique<LocalFs>(*state.fs, *state.inodes, false);
  }
  state.adapter = std::make_unique<FusePathAdapter>(*state.backend);

  // Lab1 runs single-threaded; concurrent filesystem operations are out of scope.
  fuse_argv.push_back(const_cast<char*>("-s"));
  auto ops = operations();
  int rc = fuse_main(static_cast<int>(fuse_argv.size()), fuse_argv.data(), &ops, &state);
  if (state.dev && !state.dev->flush()) { NANOFS_LOG_ERROR("flush failed"); return 1; }
  return rc;
}
