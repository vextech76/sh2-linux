#include "core/paths.h"

#include <cstdlib>

namespace lantern {
namespace fs = std::filesystem;

namespace {
fs::path xdg_dir(const EnvLookup& env, const char* var, const char* home_fallback) {
  if (auto v = env(var); v && !v->empty() && fs::path(*v).is_absolute()) return fs::path(*v);
  auto home = env("HOME");
  fs::path base = (home && !home->empty()) ? fs::path(*home) : fs::path("/tmp");
  return base / home_fallback;
}
}  // namespace

Paths resolve_paths(const EnvLookup& env) {
  Paths p;
  p.config_dir = xdg_dir(env, "XDG_CONFIG_HOME", ".config") / "lantern";
  p.data_dir = xdg_dir(env, "XDG_DATA_HOME", ".local/share") / "lantern";
  p.cache_dir = xdg_dir(env, "XDG_CACHE_HOME", ".cache") / "lantern";
  return p;
}

Paths resolve_paths() {
  return resolve_paths([](const char* name) -> std::optional<std::string> {
    if (const char* v = std::getenv(name)) return std::string(v);
    return std::nullopt;
  });
}

bool path_is_within(const fs::path& candidate, const fs::path& root) {
  auto c = fs::weakly_canonical(fs::absolute(candidate)).lexically_normal();
  auto r = fs::weakly_canonical(fs::absolute(root)).lexically_normal();
  auto rel = c.lexically_relative(r);
  if (rel.empty()) return false;
  return *rel.begin() != "..";
}

}  // namespace lantern
