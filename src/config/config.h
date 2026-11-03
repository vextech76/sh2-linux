#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "input/input_system.h"

namespace lantern {

enum class PresentMode { Fifo, Mailbox, Immediate };
enum class WindowMode { Windowed, Borderless, Exclusive };
enum class Validation { Auto, On, Off };
enum class AspectMode { Keep4x3, Stretch };
enum class RenderBackend { Vulkan, Null };

struct GraphicsConfig {
  RenderBackend backend = RenderBackend::Vulkan;
  PresentMode present_mode = PresentMode::Fifo;  // FIFO is guaranteed by Vulkan; others fall back
  WindowMode window_mode = WindowMode::Windowed;
  uint32_t width = 1280;
  uint32_t height = 960;
  uint32_t resolution_scale = 2;  // internal render scale for the emulated GS (1..8)
  uint32_t frames_in_flight = 2;  // 1..3
  AspectMode aspect = AspectMode::Keep4x3;
  Validation validation = Validation::Auto;  // Auto = on in debug builds when layers exist
  std::string preferred_gpu;                 // case-insensitive substring of the device name; empty = auto
};

struct GameConfig {
  std::filesystem::path iso_path;  // read-only source disc image (never written to)
};

struct Config {
  GraphicsConfig graphics;
  input::InputSettings input;
  GameConfig game;
  std::string log_level = "info";
};

struct ConfigResult {
  Config config;
  std::vector<std::string> warnings;  // unknown keys, out-of-range values clamped, etc.
  std::vector<std::string> errors;    // syntax/type errors; affected values keep defaults
  bool ok() const { return errors.empty(); }
};

// Parse TOML text. Missing keys keep defaults. Never throws.
ConfigResult parse_config(std::string_view text);
ConfigResult load_config(const std::filesystem::path& file);

// Serialise to TOML; parse_config(serialize_config(c)) round-trips.
std::string serialize_config(const Config& c);
bool save_config(const Config& c, const std::filesystem::path& file, std::string* error = nullptr);

std::string_view to_string(PresentMode m);
std::string_view to_string(WindowMode m);
std::string_view to_string(Validation v);
std::string_view to_string(AspectMode a);
std::string_view to_string(RenderBackend b);

}  // namespace lantern
