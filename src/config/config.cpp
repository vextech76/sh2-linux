#include "config/config.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <toml++/toml.hpp>

namespace lantern {
namespace fs = std::filesystem;
using namespace std::string_view_literals;

std::string_view to_string(PresentMode m) {
  switch (m) {
    case PresentMode::Fifo: return "fifo";
    case PresentMode::Mailbox: return "mailbox";
    case PresentMode::Immediate: return "immediate";
  }
  return "fifo";
}
std::string_view to_string(WindowMode m) {
  switch (m) {
    case WindowMode::Windowed: return "windowed";
    case WindowMode::Borderless: return "borderless";
    case WindowMode::Exclusive: return "exclusive";
  }
  return "windowed";
}
std::string_view to_string(Validation v) {
  switch (v) {
    case Validation::Auto: return "auto";
    case Validation::On: return "on";
    case Validation::Off: return "off";
  }
  return "auto";
}
std::string_view to_string(AspectMode a) { return a == AspectMode::Stretch ? "stretch" : "4:3"; }
std::string_view to_string(RenderBackend b) { return b == RenderBackend::Null ? "null" : "vulkan"; }

namespace {

struct Ctx {
  ConfigResult& r;
  void warn(std::string s) { r.warnings.push_back(std::move(s)); }
  void err(std::string s) { r.errors.push_back(std::move(s)); }
};

void check_unknown(Ctx& c, const toml::table& t, std::string_view where, std::set<std::string_view> known) {
  for (const auto& [k, v] : t) {
    (void)v;
    if (!known.contains(k.str())) c.warn(std::format("unknown key '{}{}'", where, k.str()));
  }
}

template <typename Enum, size_t N>
void read_enum(Ctx& c, const toml::table& t, std::string_view key, std::string_view where,
               const std::pair<std::string_view, Enum> (&options)[N], Enum& out) {
  const toml::node* n = t.get(key);
  if (!n) return;
  auto s = n->value<std::string>();
  if (!s) return c.err(std::format("'{}{}' must be a string", where, key));
  for (const auto& [name, value] : options)
    if (*s == name) {
      out = value;
      return;
    }
  std::string allowed;
  for (const auto& [name, value] : options) allowed += std::format("{}\"{}\"", allowed.empty() ? "" : ", ", name);
  c.err(std::format("'{}{}' = \"{}\" is not one of {}", where, key, *s, allowed));
}

void read_uint(Ctx& c, const toml::table& t, std::string_view key, std::string_view where, uint32_t lo, uint32_t hi,
               uint32_t& out) {
  const toml::node* n = t.get(key);
  if (!n) return;
  auto v = n->value_exact<int64_t>();
  if (!v) return c.err(std::format("'{}{}' must be an integer", where, key));
  auto clamped = std::clamp<int64_t>(*v, lo, hi);
  if (clamped != *v) c.warn(std::format("'{}{}' = {} out of range [{}, {}]; clamped", where, key, *v, lo, hi));
  out = static_cast<uint32_t>(clamped);
}

void read_float(Ctx& c, const toml::table& t, std::string_view key, std::string_view where, float lo, float hi,
                float& out) {
  const toml::node* n = t.get(key);
  if (!n) return;
  auto v = n->value<double>();  // accepts integers too
  if (!v) return c.err(std::format("'{}{}' must be a number", where, key));
  auto clamped = std::clamp(*v, static_cast<double>(lo), static_cast<double>(hi));
  if (clamped != *v) c.warn(std::format("'{}{}' = {} out of range [{}, {}]; clamped", where, key, *v, lo, hi));
  out = static_cast<float>(clamped);
}

void read_bool(Ctx& c, const toml::table& t, std::string_view key, std::string_view where, bool& out) {
  const toml::node* n = t.get(key);
  if (!n) return;
  auto v = n->value_exact<bool>();
  if (!v) return c.err(std::format("'{}{}' must be true or false", where, key));
  out = *v;
}

void read_string(Ctx& c, const toml::table& t, std::string_view key, std::string_view where, std::string& out) {
  const toml::node* n = t.get(key);
  if (!n) return;
  auto v = n->value_exact<std::string>();
  if (!v) return c.err(std::format("'{}{}' must be a string", where, key));
  out = *v;
}

void read_bindings(Ctx& c, const toml::table& t, std::string_view where, bool gamepad, input::BindingTable& table) {
  for (const auto& [k, v] : t) {
    auto target = input::target_from_name(k.str());
    if (!target) {
      c.warn(std::format("unknown binding target '{}{}'", where, k.str()));
      continue;
    }
    std::vector<std::string> names;
    if (auto s = v.value_exact<std::string>()) {
      names.push_back(*s);
    } else if (const auto* arr = v.as_array()) {
      bool bad = false;
      for (const auto& e : *arr) {
        if (auto s2 = e.value_exact<std::string>())
          names.push_back(*s2);
        else
          bad = true;
      }
      if (bad) c.err(std::format("'{}{}' must contain only strings", where, k.str()));
    } else {
      c.err(std::format("'{}{}' must be a string or array of strings", where, k.str()));
      continue;
    }
    table.clear(*target);
    for (const auto& name : names) {
      auto src = input::source_from_name(name);
      if (!src) {
        c.warn(std::format("'{}{}': unknown input '{}'", where, k.str(), name));
        continue;
      }
      if (input::is_gamepad_source(*src) != gamepad) {
        c.warn(std::format("'{}{}': '{}' belongs in [input.{}]", where, k.str(), name,
                           gamepad ? "keyboard_mouse" : "gamepad"));
        continue;
      }
      for (const auto& conflict : table.bind(*target, *src))
        c.warn(std::format("'{}{}': '{}' was bound to '{}'; moved", where, k.str(), name,
                           input::target_name(conflict.target)));
    }
  }
}

constexpr std::pair<std::string_view, RenderBackend> kBackends[] = {{"vulkan", RenderBackend::Vulkan},
                                                                    {"null", RenderBackend::Null}};
constexpr std::pair<std::string_view, PresentMode> kPresent[] = {
    {"fifo", PresentMode::Fifo}, {"mailbox", PresentMode::Mailbox}, {"immediate", PresentMode::Immediate}};
constexpr std::pair<std::string_view, WindowMode> kWindow[] = {
    {"windowed", WindowMode::Windowed}, {"borderless", WindowMode::Borderless}, {"exclusive", WindowMode::Exclusive}};
constexpr std::pair<std::string_view, Validation> kValidation[] = {
    {"auto", Validation::Auto}, {"on", Validation::On}, {"off", Validation::Off}};
constexpr std::pair<std::string_view, AspectMode> kAspect[] = {{"4:3", AspectMode::Keep4x3},
                                                               {"stretch", AspectMode::Stretch}};

}  // namespace

ConfigResult parse_config(std::string_view text) {
  ConfigResult r;
  Ctx c{r};
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error& e) {
    c.err(std::format("TOML syntax error at line {}, column {}: {}", e.source().begin.line, e.source().begin.column,
                      e.description()));
    return r;
  }
  Config& cfg = r.config;

  check_unknown(c, root, "", {"log_level", "graphics", "game", "input"});
  read_string(c, root, "log_level", "", cfg.log_level);
  if (cfg.log_level != "debug" && cfg.log_level != "info" && cfg.log_level != "warn" && cfg.log_level != "error") {
    c.err(std::format("'log_level' = \"{}\" is not one of debug, info, warn, error", cfg.log_level));
    cfg.log_level = "info";
  }

  if (const auto* g = root["graphics"].as_table()) {
    constexpr auto w = "graphics."sv;
    check_unknown(c, *g, w,
                  {"backend", "present_mode", "window_mode", "width", "height", "resolution_scale", "frames_in_flight",
                   "aspect", "validation", "preferred_gpu"});
    read_enum(c, *g, "backend", w, kBackends, cfg.graphics.backend);
    read_enum(c, *g, "present_mode", w, kPresent, cfg.graphics.present_mode);
    read_enum(c, *g, "window_mode", w, kWindow, cfg.graphics.window_mode);
    read_uint(c, *g, "width", w, 320, 16384, cfg.graphics.width);
    read_uint(c, *g, "height", w, 240, 16384, cfg.graphics.height);
    read_uint(c, *g, "resolution_scale", w, 1, 8, cfg.graphics.resolution_scale);
    read_uint(c, *g, "frames_in_flight", w, 1, 3, cfg.graphics.frames_in_flight);
    read_enum(c, *g, "aspect", w, kAspect, cfg.graphics.aspect);
    read_enum(c, *g, "validation", w, kValidation, cfg.graphics.validation);
    read_string(c, *g, "preferred_gpu", w, cfg.graphics.preferred_gpu);
  } else if (root.contains("graphics")) {
    c.err("'graphics' must be a table");
  }

  if (const auto* g = root["game"].as_table()) {
    check_unknown(c, *g, "game.", {"iso_path"});
    std::string iso;
    read_string(c, *g, "iso_path", "game.", iso);
    cfg.game.iso_path = iso;
  } else if (root.contains("game")) {
    c.err("'game' must be a table");
  }

  if (const auto* in = root["input"].as_table()) {
    constexpr auto w = "input."sv;
    auto& s = cfg.input;
    check_unknown(c, *in, w,
                  {"button_threshold", "mouse_sensitivity", "invert_mouse_y", "deadzone", "keyboard_mouse", "gamepad"});
    read_float(c, *in, "button_threshold", w, 0.05f, 1.0f, s.button_threshold);
    read_float(c, *in, "mouse_sensitivity", w, 0.0f, 10.0f, s.mouse_sensitivity);
    read_bool(c, *in, "invert_mouse_y", w, s.invert_mouse_y);
    if (const auto* dz = (*in)["deadzone"].as_table()) {
      constexpr auto d = "input.deadzone."sv;
      check_unknown(c, *dz, d,
                    {"left_inner", "left_outer", "right_inner", "right_outer", "trigger_inner", "trigger_outer"});
      read_float(c, *dz, "left_inner", d, 0.0f, 0.9f, s.left_stick.inner);
      read_float(c, *dz, "left_outer", d, 0.1f, 1.0f, s.left_stick.outer);
      read_float(c, *dz, "right_inner", d, 0.0f, 0.9f, s.right_stick.inner);
      read_float(c, *dz, "right_outer", d, 0.1f, 1.0f, s.right_stick.outer);
      read_float(c, *dz, "trigger_inner", d, 0.0f, 0.9f, s.triggers.inner);
      read_float(c, *dz, "trigger_outer", d, 0.1f, 1.0f, s.triggers.outer);
      for (auto* z : {&s.left_stick, &s.right_stick})
        if (z->outer <= z->inner) {
          c.warn("stick dead zone outer <= inner; using defaults");
          *z = input::StickDeadzone{};
        }
      if (s.triggers.outer <= s.triggers.inner) {
        c.warn("trigger dead zone outer <= inner; using defaults");
        s.triggers = input::TriggerDeadzone{};
      }
    }
    if (const auto* km = (*in)["keyboard_mouse"].as_table())
      read_bindings(c, *km, "input.keyboard_mouse.", false, s.keyboard_mouse);
    if (const auto* gp = (*in)["gamepad"].as_table()) read_bindings(c, *gp, "input.gamepad.", true, s.gamepad);
  } else if (root.contains("input")) {
    c.err("'input' must be a table");
  }
  return r;
}

ConfigResult load_config(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    ConfigResult r;
    r.errors.push_back(std::format("cannot read config file '{}'", file.string()));
    return r;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_config(ss.str());
}

namespace {
toml::array sources_array(const input::BindingTable& t, const input::Target& target) {
  toml::array a;
  for (const auto& s : t.sources(target)) a.push_back(input::source_name(s));
  return a;
}

toml::table bindings_table(const input::BindingTable& t) {
  toml::table out;
  // Write every target so that unbound targets are explicit ([]) and survive a round-trip.
  for (const auto& target : input::all_targets()) out.insert(input::target_name(target), sources_array(t, target));
  return out;
}
}  // namespace

std::string serialize_config(const Config& c) {
  const auto& g = c.graphics;
  toml::table graphics{{"backend", std::string(to_string(g.backend))},
                       {"present_mode", std::string(to_string(g.present_mode))},
                       {"window_mode", std::string(to_string(g.window_mode))},
                       {"width", static_cast<int64_t>(g.width)},
                       {"height", static_cast<int64_t>(g.height)},
                       {"resolution_scale", static_cast<int64_t>(g.resolution_scale)},
                       {"frames_in_flight", static_cast<int64_t>(g.frames_in_flight)},
                       {"aspect", std::string(to_string(g.aspect))},
                       {"validation", std::string(to_string(g.validation))},
                       {"preferred_gpu", g.preferred_gpu}};
  const auto& s = c.input;
  toml::table deadzone{{"left_inner", static_cast<double>(s.left_stick.inner)},
                       {"left_outer", static_cast<double>(s.left_stick.outer)},
                       {"right_inner", static_cast<double>(s.right_stick.inner)},
                       {"right_outer", static_cast<double>(s.right_stick.outer)},
                       {"trigger_inner", static_cast<double>(s.triggers.inner)},
                       {"trigger_outer", static_cast<double>(s.triggers.outer)}};
  toml::table input{{"button_threshold", static_cast<double>(s.button_threshold)},
                    {"mouse_sensitivity", static_cast<double>(s.mouse_sensitivity)},
                    {"invert_mouse_y", s.invert_mouse_y},
                    {"deadzone", std::move(deadzone)},
                    {"keyboard_mouse", bindings_table(s.keyboard_mouse)},
                    {"gamepad", bindings_table(s.gamepad)}};
  toml::table root{{"log_level", c.log_level},
                   {"graphics", std::move(graphics)},
                   {"game", toml::table{{"iso_path", c.game.iso_path.string()}}},
                   {"input", std::move(input)}};
  std::ostringstream out;
  out << "# LANTERN configuration. Generated defaults; edit freely.\n"
         "# Binding names: see docs/INPUT.md. An empty list [] unbinds a target.\n\n"
      << root << "\n";
  return out.str();
}

bool save_config(const Config& c, const fs::path& file, std::string* error) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  auto tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = std::format("cannot write '{}'", tmp.string());
      return false;
    }
    out << serialize_config(c);
    if (!out) {
      if (error) *error = std::format("write failed for '{}'", tmp.string());
      return false;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    if (error) *error = std::format("cannot replace '{}': {}", file.string(), ec.message());
    return false;
  }
  return true;
}

}  // namespace lantern
