#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>
#include <format>

#include "app/cli.h"
#include "config/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "platform/sdl_platform.h"
#include "render/renderer.h"

namespace fs = std::filesystem;
using namespace lantern;

namespace {
constexpr const char* kSub = "app";
constexpr const char* kTitle = "LANTERN - PLACEHOLDER OUTPUT (no game runtime loaded)";

log::Level parse_level(const std::string& s) {
  if (s == "debug") return log::Level::Debug;
  if (s == "warn") return log::Level::Warn;
  if (s == "error") return log::Level::Error;
  return log::Level::Info;
}

#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

// User-writable directories must never live alongside the supplied (read-only) game data.
bool check_paths_outside_game_data(const Paths& paths, const fs::path& config_file, const GameConfig& game) {
  if (game.iso_path.empty()) return true;
  const fs::path game_dir = fs::absolute(game.iso_path).parent_path();
  for (const auto& p : {paths.config_dir, paths.data_dir, paths.cache_dir, config_file.parent_path()}) {
    if (path_is_within(p, game_dir)) {
      log::error(kSub, "refusing to run: writable path '{}' is inside the game data directory '{}'", p.string(),
                 game_dir.string());
      return false;
    }
  }
  return true;
}

int run(const app::CliOptions& cli) {
  const Paths paths = resolve_paths();
  const fs::path config_file = cli.config_path ? fs::path(*cli.config_path) : paths.config_file();

  if (cli.write_default_config) {
    std::string err;
    if (!save_config(Config{}, config_file, &err)) {
      log::error(kSub, "{}", err);
      return 1;
    }
    std::printf("wrote default config to %s\n", config_file.string().c_str());
    return 0;
  }

  Config config;
  if (fs::exists(config_file)) {
    auto res = load_config(config_file);
    for (const auto& w : res.warnings) log::warn("config", "{}: {}", config_file.string(), w);
    for (const auto& e : res.errors) log::error("config", "{}: {} (using default)", config_file.string(), e);
    config = std::move(res.config);
  } else if (!cli.config_path) {
    std::string err;
    if (save_config(config, config_file, &err))
      log::info("config", "created default config at {}", config_file.string());
    else
      log::warn("config", "could not create default config: {}", err);
  } else {
    log::error("config", "config file '{}' does not exist", config_file.string());
    return 2;
  }

  if (cli.backend) config.graphics.backend = *cli.backend;
  if (cli.validation) config.graphics.validation = *cli.validation;
  if (cli.log_level) config.log_level = *cli.log_level;
  if (cli.iso_path) config.game.iso_path = *cli.iso_path;
  log::set_min_level(parse_level(config.log_level));

  if (cli.print_config) {
    std::fputs(serialize_config(config).c_str(), stdout);
    return 0;
  }
  if (!check_paths_outside_game_data(paths, config_file, config.game)) return 2;

  const bool headless = config.graphics.backend == RenderBackend::Null;
  std::string err;
  auto platform = platform::SdlPlatform::create(!headless, &err);
  if (!platform) {
    log::error(kSub, "{}", err);
    return 1;
  }
  if (!headless && !platform->create_window(config.graphics, kTitle, &err)) {
    log::error(kSub, "{}", err);
    return 1;
  }

  if (cli.list_gpus) {
    auto devices = render::describe_vulkan_devices(platform.get(), &err);
    if (!err.empty()) log::error(kSub, "{}", err);
    for (const auto& d : devices) std::printf("%s\n", d.c_str());
    return devices.empty() ? 1 : 0;
  }

  render::RendererCreateInfo rci;
  rci.graphics = config.graphics;
  rci.surface = headless ? nullptr : platform.get();
  rci.cache_dir = paths.cache_dir;
  rci.debug_build = kDebugBuild;
  auto renderer = render::create_renderer(rci, &err);
  if (!renderer) {
    log::error("render", "{}", err);
    return 1;
  }
  const auto& info = renderer->info();
  log::info("render", "backend={} device='{}' driver='{}' api={} present={} validation={}", info.backend, info.device,
            info.driver, info.api_version, info.present_mode, info.validation ? "on" : "off");
  log::warn(kSub, "no game runtime is implemented yet; displaying the PLACEHOLDER pattern");

  input::InputSystem input(config.input);
  const uint64_t start = SDL_GetTicksNS();
  uint16_t last_buttons = 0;
  uint64_t frames = 0;

  while (true) {
    auto pump = platform->pump(input);
    if (pump.quit) break;
    if (pump.resized) renderer->notify_resize();

    for (auto action : input.take_host_actions()) {
      switch (action) {
        case input::HostAction::ToggleFullscreen: platform->toggle_fullscreen(); break;
        case input::HostAction::ToggleMouseCapture: platform->set_mouse_capture(!platform->mouse_captured()); break;
        case input::HostAction::Quit: return 0;
        default: break;
      }
    }

    const input::PadState pad = input.sample();
    if (pad.buttons != last_buttons) {
      log::debug("input", "pad buttons=0x{:04x} lx={} ly={} rx={} ry={}", pad.buttons, pad.axes[0], pad.axes[1],
                 pad.axes[2], pad.axes[3]);
      last_buttons = pad.buttons;
    }

    if (cli.exercise_swapchain && frames % 20 == 19) {
      static constexpr std::pair<uint32_t, uint32_t> kSizes[] = {{640, 480}, {1600, 900}, {333, 777}, {1280, 960}};
      static constexpr PresentMode kModes[] = {PresentMode::Mailbox, PresentMode::Immediate, PresentMode::Fifo};
      const auto step = frames / 20;
      const auto [w, h] = kSizes[step % std::size(kSizes)];
      platform->set_window_size(w, h);
      renderer->set_present_mode(kModes[step % std::size(kModes)]);
    }

    const float t = static_cast<float>(SDL_GetTicksNS() - start) / 1e9f;
    if (renderer->begin_frame() == render::FrameStatus::Ready) {
      renderer->draw_placeholder({t});
      renderer->end_frame();
      ++frames;
    } else {
      SDL_DelayNS(10'000'000);  // minimised or recreating; don't spin
    }
    if (headless) SDL_DelayNS(16'666'667);
    if (cli.frames && frames >= cli.frames) break;
  }

  renderer->wait_idle();
  log::info(kSub, "presented {} frames", renderer->frames_presented());
  if (auto n = render::vulkan_validation_error_count(); n > 0) {
    log::error("render", "{} Vulkan validation error(s) were reported", n);
    return 3;
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  auto cli = app::parse_cli(args);
  if (!cli.error.empty()) {
    std::fprintf(stderr, "lantern: %s\n\n%s", cli.error.c_str(), app::cli_usage().c_str());
    return 2;
  }
  if (cli.options.help) {
    std::fputs(app::cli_usage().c_str(), stdout);
    return 0;
  }
  if (cli.options.version) {
    std::printf("lantern %s\n", LANTERN_VERSION);
    return 0;
  }
  try {
    return run(cli.options);
  } catch (const std::exception& e) {
    log::error(kSub, "fatal: {}", e.what());
    return 1;
  }
}
