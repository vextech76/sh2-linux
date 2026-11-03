#pragma once

#include <format>
#include <string_view>

namespace lantern::log {

enum class Level { Debug, Info, Warn, Error };

void set_min_level(Level level);
Level min_level();
void write(Level level, std::string_view subsystem, std::string_view message);

template <typename... Args>
void debug(std::string_view sub, std::format_string<Args...> fmt, Args&&... args) {
  if (min_level() <= Level::Debug) write(Level::Debug, sub, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void info(std::string_view sub, std::format_string<Args...> fmt, Args&&... args) {
  if (min_level() <= Level::Info) write(Level::Info, sub, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void warn(std::string_view sub, std::format_string<Args...> fmt, Args&&... args) {
  if (min_level() <= Level::Warn) write(Level::Warn, sub, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void error(std::string_view sub, std::format_string<Args...> fmt, Args&&... args) {
  write(Level::Error, sub, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace lantern::log
