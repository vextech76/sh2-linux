#include "platform/sdl_platform.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <map>

#include "core/log.h"

namespace lantern::platform {
namespace {
constexpr const char* kSub = "platform";

using input::GamepadAxis;
using input::GamepadButton;
static_assert(static_cast<int>(GamepadButton::South) == SDL_GAMEPAD_BUTTON_SOUTH);
static_assert(static_cast<int>(GamepadButton::DpadRight) == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
static_assert(static_cast<int>(GamepadButton::Touchpad) == SDL_GAMEPAD_BUTTON_TOUCHPAD);
static_assert(static_cast<int>(GamepadAxis::LeftX) == SDL_GAMEPAD_AXIS_LEFTX);
static_assert(static_cast<int>(GamepadAxis::RightTrigger) == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
static_assert(SDL_BUTTON_LEFT == 1 && SDL_BUTTON_MIDDLE == 2 && SDL_BUTTON_RIGHT == 3);
static_assert(SDL_SCANCODE_A == 4 && SDL_SCANCODE_LCTRL == 224);  // HID usage == SDL scancode

float axis_to_float(Sint16 v, bool trigger) {
  if (trigger) return std::clamp(static_cast<float>(v) / 32767.0f, 0.0f, 1.0f);
  return std::clamp(static_cast<float>(v) / 32767.0f, -1.0f, 1.0f);
}
}  // namespace

struct SdlPlatform::Impl {
  std::map<SDL_JoystickID, SDL_Gamepad*> gamepads;
};

std::unique_ptr<SdlPlatform> SdlPlatform::create(bool with_video, std::string* error) {
  SDL_SetHint(SDL_HINT_APP_NAME, "LANTERN");
  SDL_InitFlags flags = SDL_INIT_GAMEPAD | (with_video ? SDL_INIT_VIDEO : 0u);
  if (!SDL_Init(flags)) {
    if (error) *error = std::string("SDL_Init failed: ") + SDL_GetError();
    return nullptr;
  }
  std::unique_ptr<SdlPlatform> p(new SdlPlatform());
  p->video_ = with_video;
  p->impl_ = std::make_unique<Impl>();
  if (with_video && !SDL_Vulkan_LoadLibrary(nullptr)) {
    if (error) *error = std::string("cannot load the Vulkan loader library: ") + SDL_GetError();
    return nullptr;
  }
  return p;
}

SdlPlatform::~SdlPlatform() {
  if (impl_)
    for (auto& [id, pad] : impl_->gamepads) SDL_CloseGamepad(pad);
  if (window_) SDL_DestroyWindow(window_);
  if (video_) SDL_Vulkan_UnloadLibrary();
  SDL_Quit();
}

bool SdlPlatform::create_window(const GraphicsConfig& g, const std::string& title, std::string* error) {
  SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  window_ = SDL_CreateWindow(title.c_str(), static_cast<int>(g.width), static_cast<int>(g.height), flags);
  if (!window_) {
    if (error) *error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
    return false;
  }
  SDL_SetWindowMinimumSize(window_, 320, 240);
  if (g.window_mode != WindowMode::Windowed) fullscreen_mode_ = g.window_mode;
  set_window_mode(g.window_mode);
  return true;
}

void SdlPlatform::set_title(const std::string& title) {
  if (window_) SDL_SetWindowTitle(window_, title.c_str());
}

void SdlPlatform::set_window_size(uint32_t width, uint32_t height) {
  if (window_) SDL_SetWindowSize(window_, static_cast<int>(width), static_cast<int>(height));
}

void SdlPlatform::set_window_mode(WindowMode mode) {
  if (!window_) return;
  if (mode == WindowMode::Windowed) {
    SDL_SetWindowFullscreen(window_, false);
    fullscreen_ = false;
    return;
  }
  if (mode == WindowMode::Exclusive) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    SDL_DisplayMode closest;
    SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
    if (SDL_GetClosestFullscreenDisplayMode(display, w, h, 0.0f, true, &closest)) {
      SDL_SetWindowFullscreenMode(window_, &closest);
    } else {
      log::warn(kSub, "no exclusive mode near {}x{} ({}); using borderless", w, h, SDL_GetError());
      SDL_SetWindowFullscreenMode(window_, nullptr);
    }
  } else {
    SDL_SetWindowFullscreenMode(window_, nullptr);  // borderless desktop
  }
  if (!SDL_SetWindowFullscreen(window_, true)) log::warn(kSub, "fullscreen failed: {}", SDL_GetError());
  fullscreen_ = true;
}

void SdlPlatform::toggle_fullscreen() { set_window_mode(fullscreen_ ? WindowMode::Windowed : fullscreen_mode_); }

void SdlPlatform::set_mouse_capture(bool captured) {
  if (!window_) return;
  if (SDL_SetWindowRelativeMouseMode(window_, captured))
    mouse_captured_ = captured;
  else
    log::warn(kSub, "mouse capture failed: {}", SDL_GetError());
}

void SdlPlatform::open_gamepad(uint32_t id, input::InputSystem& input) {
  if (impl_->gamepads.contains(id)) return;
  SDL_Gamepad* pad = SDL_OpenGamepad(id);
  if (!pad) {
    log::warn(kSub, "cannot open gamepad {}: {}", id, SDL_GetError());
    return;
  }
  impl_->gamepads[id] = pad;
  const char* name = SDL_GetGamepadName(pad);
  input.on_gamepad_added(id, name ? name : "unknown gamepad");
  log::info(kSub, "gamepad connected: {} (id {}){}", name ? name : "?", id,
            input.active_gamepad() == id ? " [active]" : "");
}

void SdlPlatform::close_gamepad(uint32_t id, input::InputSystem& input) {
  auto it = impl_->gamepads.find(id);
  if (it == impl_->gamepads.end()) return;
  SDL_CloseGamepad(it->second);
  impl_->gamepads.erase(it);
  input.on_gamepad_removed(id);
  log::info(kSub, "gamepad disconnected (id {})", id);
  if (auto active = input.active_gamepad()) log::info(kSub, "active gamepad is now id {}", *active);
}

PumpResult SdlPlatform::pump(input::InputSystem& input) {
  PumpResult r;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED: r.quit = true; break;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_RESTORED:
      case SDL_EVENT_WINDOW_MAXIMIZED: r.resized = true; break;
      case SDL_EVENT_WINDOW_FOCUS_LOST: input.release_all(); break;
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP:
        if (!e.key.repeat) input.on_key(static_cast<uint16_t>(e.key.scancode), e.key.down);
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: input.on_mouse_button(e.button.button, e.button.down); break;
      case SDL_EVENT_MOUSE_MOTION:
        // Only relative (captured) motion drives the virtual pad, so moving the cursor across
        // the window never nudges a stick.
        if (mouse_captured_) input.on_mouse_motion(e.motion.xrel, e.motion.yrel);
        break;
      case SDL_EVENT_GAMEPAD_ADDED: open_gamepad(e.gdevice.which, input); break;
      case SDL_EVENT_GAMEPAD_REMOVED: close_gamepad(e.gdevice.which, input); break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      case SDL_EVENT_GAMEPAD_BUTTON_UP:
        input.on_gamepad_button(e.gbutton.which, static_cast<GamepadButton>(e.gbutton.button), e.gbutton.down);
        break;
      case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        auto axis = static_cast<GamepadAxis>(e.gaxis.axis);
        bool trigger = axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger;
        input.on_gamepad_axis(e.gaxis.which, axis, axis_to_float(e.gaxis.value, trigger));
        break;
      }
      default: break;
    }
  }
  return r;
}

std::vector<std::string> SdlPlatform::vulkan_instance_extensions() const {
  Uint32 count = 0;
  const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
  std::vector<std::string> out;
  for (Uint32 i = 0; names && i < count; ++i) out.emplace_back(names[i]);
  return out;
}

bool SdlPlatform::create_vulkan_surface(void* instance, uint64_t* surface, std::string* error) const {
  VkSurfaceKHR s{};
  if (!window_ || !SDL_Vulkan_CreateSurface(window_, static_cast<VkInstance>(instance), nullptr, &s)) {
    if (error) *error = window_ ? SDL_GetError() : "no window";
    return false;
  }
  *surface = reinterpret_cast<uint64_t>(s);
  return true;
}

render::Extent SdlPlatform::drawable_extent() const {
  int w = 0, h = 0;
  if (window_) SDL_GetWindowSizeInPixels(window_, &w, &h);
  if (window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) return {0, 0};
  return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

}  // namespace lantern::platform
