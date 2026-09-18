#pragma once
#include <sstream>
#include <string>
#include <string_view>
#include "nanofs/error.h"

namespace nanofs::log {

enum class Level { Debug, Info, Warn, Error };

// Set the global log threshold and output target. An empty file_path means
// stderr; otherwise the file is opened in append mode, and Error::IoError is
// returned if the open fails. Call this once, before starting the server/fuse
// main loop; init() itself is not thread-safe, but enabled()/write() are.
Result<void> init(Level level, const std::string& file_path = "");

// "debug"/"info"/"warn"/"error" -> Level; case-sensitive, anything else
// returns Error::Invalid.
Result<Level> parse_level(std::string_view s);

// Whether messages at this level get printed (level >= current threshold).
bool enabled(Level level);

// Format as "[LEVEL] msg\n" and write it to the current output. Thread-safe
// (takes a lock internally).
void write(Level level, const std::string& msg);

// Format one log line (including the trailing '\n'). With color=true the
// [LEVEL] tag is colored using ANSI escape codes and the message itself is not;
// with color=false the output is plain text "[LEVEL] msg\n". A pure function,
// which makes it easy to test.
std::string format_line(Level level, const std::string& msg, bool color);

}  // namespace nanofs::log

#define NANOFS_LOG(level, expr)                             \
  do {                                                       \
    if (::nanofs::log::enabled(level)) {                     \
      std::ostringstream nanofs_log_oss_;                    \
      nanofs_log_oss_ << expr;                                \
      ::nanofs::log::write(level, nanofs_log_oss_.str());     \
    }                                                          \
  } while (0)

#define NANOFS_LOG_DEBUG(expr) NANOFS_LOG(::nanofs::log::Level::Debug, expr)
#define NANOFS_LOG_INFO(expr)  NANOFS_LOG(::nanofs::log::Level::Info, expr)
#define NANOFS_LOG_WARN(expr)  NANOFS_LOG(::nanofs::log::Level::Warn, expr)
#define NANOFS_LOG_ERROR(expr) NANOFS_LOG(::nanofs::log::Level::Error, expr)
