#pragma once

#include <memory>
#include <string>

#include "config/config.h"
#include "input/input_system.h"
#include "render/renderer.h"

struct SDL_Window;

namespace lantern::platform {

struct PumpResult {
  bool quit = false;
  bool resized = false;
};

// SDL3-backed window + event pump. Translates SDL events into InputSystem calls so the input
// core stays SDL-free. Owns gamepad handles and handles hot-plug.
class SdlPlatform final : public render::SurfaceSource {
 public:
  ~SdlPlatform() override;

  // Initialise SDL. `with_video` false = headless (gamepads only, no window).
  static std::unique_ptr<SdlPlatform> create(bool with_video, std::string* error);

  bool create_window(const GraphicsConfig& g, const std::string& title, std::string* error);
  void set_title(const std::string& title);
  void set_window_size(uint32_t width, uint32_t height);
  PumpResult pump(input::InputSystem& input);

  void set_window_mode(WindowMode mode);
  void toggle_fullscreen();
  void set_mouse_capture(bool captured);
  bool mouse_captured() const { return mouse_captured_; }

  // render::SurfaceSource
  std::vector<std::string> vulkan_instance_extensions() const override;
  bool create_vulkan_surface(void* instance, uint64_t* surface, std::string* error) const override;
  render::Extent drawable_extent() const override;

 private:
  SdlPlatform() = default;
  void open_gamepad(uint32_t id, input::InputSystem& input);
  void close_gamepad(uint32_t id, input::InputSystem& input);

  bool video_ = false;
  SDL_Window* window_ = nullptr;
  WindowMode fullscreen_mode_ = WindowMode::Borderless;
  bool fullscreen_ = false;
  bool mouse_captured_ = false;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace lantern::platform
