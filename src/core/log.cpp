#include "core/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace lantern::log {
namespace {
std::atomic<Level> g_min{Level::Info};
std::mutex g_mutex;

const char* level_tag(Level level) {
  switch (level) {
    case Level::Debug: return "debug";
    case Level::Info: return "info ";
    case Level::Warn: return "warn ";
    case Level::Error: return "error";
  }
  return "?";
}
}  // namespace

void set_min_level(Level level) { g_min.store(level); }
Level min_level() { return g_min.load(); }

void write(Level level, std::string_view subsystem, std::string_view message) {
  std::lock_guard lock(g_mutex);
  std::FILE* out = level >= Level::Warn ? stderr : stdout;
  std::fprintf(out, "[%s] %.*s: %.*s\n", level_tag(level), static_cast<int>(subsystem.size()), subsystem.data(),
               static_cast<int>(message.size()), message.data());
  std::fflush(out);
}

}  // namespace lantern::log
