#include <doctest/doctest.h>

#include "render/renderer.h"

using namespace lantern;
using namespace lantern::render;

TEST_CASE("null renderer counts frames and needs no surface") {
  RendererCreateInfo ci;
  ci.graphics.backend = RenderBackend::Null;
  std::string err;
  auto r = create_renderer(ci, &err);
  REQUIRE(r);
  CHECK(r->info().backend == "null");
  for (int i = 0; i < 3; ++i) {
    REQUIRE(r->begin_frame() == FrameStatus::Ready);
    r->draw_placeholder({});
    r->end_frame();
  }
  r->end_frame();  // unmatched end is ignored
  CHECK(r->frames_presented() == 3);
}

TEST_CASE("vulkan backend without a surface fails with a clear error") {
  RendererCreateInfo ci;
  ci.graphics.backend = RenderBackend::Vulkan;
  std::string err;
  CHECK_FALSE(create_renderer(ci, &err));
  CHECK(err.find("surface") != std::string::npos);
}
