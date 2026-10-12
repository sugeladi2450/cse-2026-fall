// nanofs server entry point
//
// Sets up the block device / FS layer, starts the librpc server, registers the
// fs_* methods from handlers.cc and listens on the given host:port. Provided;
// lab 1 changes nothing under server/.

#include "nanofs/handlers.h"
#include "nanofs/fs.h"
#include "nanofs/inode.h"
#include "nanofs/block.h"
#include "nanofs/journal.h"
#include "nanofs/locked_fs.h"
#include "nanofs/txfs.h"
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
  std::unique_ptr<SimulatedBlockDevice> journal_dev;
  if (!image.empty()) {
    auto d = SimulatedBlockDevice::open_file(image + ".journal", block_size, 256);
    if (!d) { NANOFS_LOG_ERROR("open journal failed: " << image << ".journal"); return 1; }
    journal_dev = std::move(*d);
  } else {
    journal_dev = std::make_unique<SimulatedBlockDevice>(block_size, 256);
  }
  JournaledBlockDevice journal(*dev, *journal_dev, 192);
  if (auto r = journal.recover(); !r) {
    NANOFS_LOG_ERROR("journal recovery failed"); return 1;
  }
  InodeManager inodes(journal, num_inodes);
  if (do_format) {
    if (auto r = inodes.format(); !r) { NANOFS_LOG_ERROR("format failed"); return 1; }
    if (auto r = journal.checkpoint(); !r) {
      NANOFS_LOG_ERROR("post-format checkpoint failed"); return 1;
    }
  }
#ifdef NANOFS_LAB2B
  LockedFs fs(inodes, journal, num_inodes);
#else
  TxFs fs(inodes, journal);
#endif

  rpc::server srv((uint16_t)port);
  register_fs_handlers(srv, fs, inodes);
  NANOFS_LOG_INFO("nanofsd listening on :" << port);

  // One RPC worker serializes requests. Clean shutdown checkpoints the image.
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
  if (auto r = journal.checkpoint(); !r) {
    NANOFS_LOG_ERROR("checkpoint on stop failed"); return 1;
  }
  NANOFS_LOG_INFO("nanofsd stopped, image checkpointed");
  return 0;
}
