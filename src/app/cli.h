#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config/config.h"

namespace lantern::app {

struct CliOptions {
  std::optional<std::string> config_path;
  std::optional<RenderBackend> backend;
  std::optional<Validation> validation;
  std::optional<std::string> log_level;
  std::optional<std::string> iso_path;
  uint64_t frames = 0;              // 0 = run until closed
  bool exercise_swapchain = false;  // test hook: resize window + switch present modes while running
  bool list_gpus = false;
  bool print_config = false;
  bool write_default_config = false;
  bool help = false;
  bool version = false;
};

struct CliResult {
  CliOptions options;
  std::string error;  // non-empty on failure
};

CliResult parse_cli(const std::vector<std::string>& args);
std::string cli_usage();

}  // namespace lantern::app
