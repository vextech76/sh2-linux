#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "config/config.h"

namespace lantern::render {

struct Extent {
  uint32_t width = 0;
  uint32_t height = 0;
};

// Window-system hook the renderer needs. Implemented by the platform layer so render code
// never includes SDL; a different windowing library only has to implement this.
class SurfaceSource {
 public:
  virtual ~SurfaceSource() = default;
  virtual std::vector<std::string> vulkan_instance_extensions() const = 0;
  // `instance` is a VkInstance; writes a VkSurfaceKHR handle to `surface`.
  virtual bool create_vulkan_surface(void* instance, uint64_t* surface, std::string* error) const = 0;
  virtual Extent drawable_extent() const = 0;
};

struct RendererInfo {
  std::string backend;
  std::string device;
  std::string driver;
  std::string api_version;
  std::string present_mode;
  bool validation = false;
};

enum class FrameStatus {
  Ready,    // a frame was begun; call end_frame()
  Skipped,  // nothing to draw into (minimised, swapchain being recreated); do not call end_frame()
};

struct PlaceholderParams {
  float time_seconds = 0.0f;
};

// Backend-neutral renderer. The interface is intentionally small: today it presents a visibly
// marked placeholder. The GS translator (see docs/DESIGN.md §Renderer) extends it with guest
// framebuffer/texture/draw-batch submission once that contract is pinned down.
class Renderer {
 public:
  virtual ~Renderer() = default;
  virtual const RendererInfo& info() const = 0;
  virtual FrameStatus begin_frame() = 0;
  virtual void draw_placeholder(const PlaceholderParams& params) = 0;
  // Emulated display output: set the image (copied) before begin_frame(), then draw it inside the
  // frame. Upload happens in begin_frame so it is ordered before rendering.
  virtual void set_frame_image(const uint32_t* rgba, uint32_t width, uint32_t height) = 0;
  virtual void draw_frame_image() = 0;
  virtual void end_frame() = 0;
  virtual void notify_resize() = 0;
  virtual void set_present_mode(PresentMode mode) = 0;
  virtual void wait_idle() = 0;
  virtual uint64_t frames_presented() const = 0;
};

struct RendererCreateInfo {
  GraphicsConfig graphics;
  SurfaceSource* surface = nullptr;  // required for Vulkan, ignored by Null
  std::filesystem::path cache_dir;   // pipeline cache location (may be empty)
  bool debug_build = false;
};

// Returns nullptr and fills `error` with a user-facing explanation on failure.
std::unique_ptr<Renderer> create_renderer(const RendererCreateInfo& info, std::string* error);

// Headless backend for tests and CI: no GPU, no window.
std::unique_ptr<Renderer> create_null_renderer();

// Implemented by the Vulkan backend; enumerates devices and why each is (un)suitable.
std::vector<std::string> describe_vulkan_devices(SurfaceSource* surface, std::string* error);

// Number of validation-layer errors reported in this process (0 if validation is off).
uint64_t vulkan_validation_error_count();

}  // namespace lantern::render
