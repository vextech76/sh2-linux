#include "render/renderer.h"

namespace lantern::render {
namespace {

class NullRenderer final : public Renderer {
 public:
  NullRenderer() { info_ = {"null", "none", "none", "n/a", "none", false}; }
  const RendererInfo& info() const override { return info_; }
  FrameStatus begin_frame() override {
    in_frame_ = true;
    return FrameStatus::Ready;
  }
  void draw_placeholder(const PlaceholderParams&) override {}
  void end_frame() override {
    if (in_frame_) ++frames_;
    in_frame_ = false;
  }
  void notify_resize() override {}
  void set_present_mode(PresentMode) override {}
  void wait_idle() override {}
  uint64_t frames_presented() const override { return frames_; }

 private:
  RendererInfo info_;
  uint64_t frames_ = 0;
  bool in_frame_ = false;
};

}  // namespace

std::unique_ptr<Renderer> create_null_renderer() { return std::make_unique<NullRenderer>(); }

}  // namespace lantern::render
