#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace lantern {

// All writable locations live under XDG base directories, never next to supplied game data.
struct Paths {
  std::filesystem::path config_dir;  // $XDG_CONFIG_HOME/lantern
  std::filesystem::path data_dir;    // $XDG_DATA_HOME/lantern   (extracted data, memory cards)
  std::filesystem::path cache_dir;   // $XDG_CACHE_HOME/lantern  (pipeline cache)

  std::filesystem::path config_file() const { return config_dir / "config.toml"; }
  std::filesystem::path extracted_dir() const { return data_dir / "extracted"; }
  std::filesystem::path memcard_dir() const { return data_dir / "memcards"; }
};

using EnvLookup = std::function<std::optional<std::string>(const char*)>;

// Resolve paths from the environment. `env` is injectable for tests.
Paths resolve_paths(const EnvLookup& env);
Paths resolve_paths();

// True if `candidate` is `root` or lies inside it (lexically, after normalisation).
bool path_is_within(const std::filesystem::path& candidate, const std::filesystem::path& root);

}  // namespace lantern
