#include <doctest/doctest.h>

#include <map>

#include "core/paths.h"

using namespace lantern;

namespace {
EnvLookup env_of(std::map<std::string, std::string> m) {
  return [m](const char* k) -> std::optional<std::string> {
    auto it = m.find(k);
    if (it == m.end()) return std::nullopt;
    return it->second;
  };
}
}  // namespace

TEST_CASE("XDG variables are honoured") {
  auto p = resolve_paths(env_of(
      {{"HOME", "/home/u"}, {"XDG_CONFIG_HOME", "/cfg"}, {"XDG_DATA_HOME", "/data"}, {"XDG_CACHE_HOME", "/cache"}}));
  CHECK(p.config_dir == "/cfg/lantern");
  CHECK(p.data_dir == "/data/lantern");
  CHECK(p.cache_dir == "/cache/lantern");
  CHECK(p.config_file() == "/cfg/lantern/config.toml");
  CHECK(p.memcard_dir() == "/data/lantern/memcards");
}

TEST_CASE("falls back to HOME, ignoring relative XDG values per spec") {
  auto p = resolve_paths(env_of({{"HOME", "/home/u"}, {"XDG_CONFIG_HOME", "relative/dir"}}));
  CHECK(p.config_dir == "/home/u/.config/lantern");
  CHECK(p.data_dir == "/home/u/.local/share/lantern");
  CHECK(p.cache_dir == "/home/u/.cache/lantern");
}

TEST_CASE("path_is_within") {
  CHECK(path_is_within("/a/b/c", "/a/b"));
  CHECK(path_is_within("/a/b", "/a/b"));
  CHECK_FALSE(path_is_within("/a/bc", "/a/b"));
  CHECK_FALSE(path_is_within("/a", "/a/b"));
  CHECK_FALSE(path_is_within("/a/b/../x", "/a/b"));
}
