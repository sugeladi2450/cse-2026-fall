// nanofs server entry point
//
// Sets up the block device / FS layer, starts the librpc server, registers the
// fs_* methods from handlers.cc and listens on the given host:port. Provided;
// lab 1 changes nothing under server/.

#include "nanofs/handlers.h"
#include "nanofs/fs.h"
#include "nanofs/inode.h"
#include "nanofs/block.h"
#include "nanofs/log.h"
#include <rpc/server.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <cstdio>

using namespace nanofs;

static void usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [--port n] [--block-size n] [--num-blocks n] [--num-inodes n] "
               "[--image path] [--format] [--log-level debug|info|warn|error] [--log-file path]\n",
               prog);
}

// Minimal --flag value parsing (provided code).
int main(int argc, char** argv) {
  int port = 8080; uint32_t block_size = 4096, num_blocks = 4096, num_inodes = 1024;
  std::string image, log_file, log_level_str = "info";
  bool do_format = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
    if (a == "--port") port = std::atoi(next().c_str());
    else if (a == "--block-size") block_size = (uint32_t)std::atoi(next().c_str());
    else if (a == "--num-blocks") num_blocks = (uint32_t)std::atoi(next().c_str());
    else if (a == "--num-inodes") num_inodes = (uint32_t)std::atoi(next().c_str());
    else if (a == "--image") image = next();
    else if (a == "--format") do_format = true;
    else if (a == "--log-level") { log_level_str = next(); if (log_level_str.empty()) { usage(argv[0]); return 2; } }
    else if (a == "--log-file") { log_file = next(); if (log_file.empty()) { usage(argv[0]); return 2; } }
  }

  auto level = log::parse_level(log_level_str);
  if (!level) { std::fprintf(stderr, "bad --log-level '%s' (want debug|info|warn|error)\n", log_level_str.c_str()); return 2; }
  if (auto r = log::init(*level, log_file); !r) {
    std::fprintf(stderr, "failed to open --log-file '%s'\n", log_file.c_str()); return 1;
  }

  std::unique_ptr<SimulatedBlockDevice> dev;
  if (!image.empty()) {
    auto d = SimulatedBlockDevice::open_file(image, block_size, num_blocks);
    if (!d) { NANOFS_LOG_ERROR("open image failed: " << image); return 1; }
    dev = std::move(*d);
  } else {
    dev = std::make_unique<SimulatedBlockDevice>(block_size, num_blocks);
    do_format = true;   // the memory backend starts fresh every run => must format
  }
  InodeManager inodes(*dev, num_inodes);
  if (do_format && !inodes.format()) { NANOFS_LOG_ERROR("format failed"); return 1; }
  FsLayer fs(inodes, *dev);

  rpc::server srv((uint16_t)port);
  register_fs_handlers(srv, fs, inodes);
  NANOFS_LOG_INFO("nanofsd listening on :" << port);

  // One RPC worker serializes file operations. Clean shutdown flushes the image.
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  sigaddset(&stop_signals, SIGINT);
  sigaddset(&stop_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);   // inherited by the worker thread
  srv.async_run(1);
  int sig = 0;
  sigwait(&stop_signals, &sig);
  NANOFS_LOG_INFO("nanofsd stopping on signal " << sig);
  srv.stop();
  if (auto r = dev->flush(); !r) { NANOFS_LOG_ERROR("flush on stop failed"); return 1; }
  NANOFS_LOG_INFO("nanofsd stopped, image flushed");
  return 0;
}
