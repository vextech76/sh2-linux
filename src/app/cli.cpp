#include "app/cli.h"

#include <charconv>
#include <format>

namespace lantern::app {

std::string cli_usage() {
  return "Usage: lantern [options]\n"
         "  --config PATH            use this config file (default: $XDG_CONFIG_HOME/lantern/config.toml)\n"
         "  --backend vulkan|null    renderer backend override\n"
         "  --validation auto|on|off Vulkan validation layer override\n"
         "  --log-level LEVEL        debug|info|warn|error\n"
         "  --iso PATH               game disc image (read-only)\n"
         "  --frames N               exit after N presented frames (smoke tests)\n"
         "  --exercise-swapchain     test hook: resize the window and switch present modes while running\n"
         "  --list-gpus              list Vulkan devices and suitability, then exit\n"
         "  --print-config           print the effective configuration, then exit\n"
         "  --write-default-config   (over)write the config file with defaults, then exit\n"
         "  --version                print version\n"
         "  -h, --help               this help\n";
}

CliResult parse_cli(const std::vector<std::string>& args) {
  CliResult r;
  auto& o = r.options;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&](std::string_view flag) -> std::optional<std::string> {
      if (i + 1 >= args.size()) {
        r.error = std::format("{} requires a value", flag);
        return std::nullopt;
      }
      return args[++i];
    };
    if (a == "-h" || a == "--help") {
      o.help = true;
    } else if (a == "--version") {
      o.version = true;
    } else if (a == "--exercise-image") {
      o.exercise_image = true;
    } else if (a == "--exercise-swapchain") {
      o.exercise_swapchain = true;
    } else if (a == "--list-gpus") {
      o.list_gpus = true;
    } else if (a == "--print-config") {
      o.print_config = true;
    } else if (a == "--write-default-config") {
      o.write_default_config = true;
    } else if (a == "--config") {
      if (auto v = value(a)) o.config_path = *v;
    } else if (a == "--iso") {
      if (auto v = value(a)) o.iso_path = *v;
    } else if (a == "--log-level") {
      if (auto v = value(a)) {
        if (*v != "debug" && *v != "info" && *v != "warn" && *v != "error") r.error = "invalid --log-level " + *v;
        o.log_level = *v;
      }
    } else if (a == "--backend") {
      if (auto v = value(a)) {
        if (*v == "vulkan")
          o.backend = RenderBackend::Vulkan;
        else if (*v == "null")
          o.backend = RenderBackend::Null;
        else
          r.error = "invalid --backend " + *v;
      }
    } else if (a == "--validation") {
      if (auto v = value(a)) {
        if (*v == "auto")
          o.validation = Validation::Auto;
        else if (*v == "on")
          o.validation = Validation::On;
        else if (*v == "off")
          o.validation = Validation::Off;
        else
          r.error = "invalid --validation " + *v;
      }
    } else if (a == "--frames") {
      if (auto v = value(a)) {
        auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), o.frames);
        if (ec != std::errc{} || p != v->data() + v->size()) r.error = "invalid --frames " + *v;
      }
    } else {
      r.error = "unknown option " + a;
    }
    if (!r.error.empty()) break;
  }
  return r;
}

}  // namespace lantern::app
