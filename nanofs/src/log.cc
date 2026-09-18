#include "nanofs/log.h"
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>

namespace nanofs::log {
namespace {
std::mutex g_mutex;
Level g_level = Level::Info;
std::ofstream g_file;
std::ostream* g_sink = &std::cerr;
bool g_color = false;

std::string_view level_name(Level level) {
  switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO";
    case Level::Warn:  return "WARN";
    case Level::Error: return "ERROR";
  }
  return "INFO";
}

// ANSI color code for each level: Debug=dim, Info=green, Warn=yellow,
// Error=bold red.
std::string_view level_color(Level level) {
  switch (level) {
    case Level::Debug: return "\x1b[2m";
    case Level::Info:  return "\x1b[32m";
    case Level::Warn:  return "\x1b[33m";
    case Level::Error: return "\x1b[1;31m";
  }
  return "\x1b[0m";
}
}  // namespace

Result<void> init(Level level, const std::string& file_path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_level = level;
  if (file_path.empty()) {
    g_sink = &std::cerr;
    // Color only when writing to a terminal and NO_COLOR is unset (the
    // no-color.org convention: merely setting it disables color).
    g_color = isatty(STDERR_FILENO) && std::getenv("NO_COLOR") == nullptr;
    return {};
  }
  g_file.close();
  g_file.clear();
  g_file.open(file_path, std::ios::app);
  if (!g_file.is_open()) return std::unexpected(Error::IoError);
  g_sink = &g_file;
  g_color = false;  // never write ANSI escape sequences into a file
  return {};
}

Result<Level> parse_level(std::string_view s) {
  if (s == "debug") return Level::Debug;
  if (s == "info") return Level::Info;
  if (s == "warn") return Level::Warn;
  if (s == "error") return Level::Error;
  return std::unexpected(Error::Invalid);
}

bool enabled(Level level) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return level >= g_level;
}

std::string format_line(Level level, const std::string& msg, bool color) {
  std::string out;
  if (color) out += level_color(level);
  out += "[";
  out += level_name(level);
  out += "]";
  if (color) out += "\x1b[0m";
  out += " ";
  out += msg;
  out += "\n";
  return out;
}

void write(Level level, const std::string& msg) {
  std::lock_guard<std::mutex> lock(g_mutex);
  (*g_sink) << format_line(level, msg, g_color);
  g_sink->flush();
}

}  // namespace nanofs::log
