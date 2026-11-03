#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>

#include "config/config.h"

using namespace lantern;
using namespace lantern::input;

namespace {
bool any_contains(const std::vector<std::string>& v, std::string_view needle) {
  return std::any_of(v.begin(), v.end(), [&](const auto& s) { return s.find(needle) != std::string::npos; });
}
}  // namespace

TEST_CASE("empty config yields defaults") {
  auto r = parse_config("");
  CHECK(r.ok());
  CHECK(r.warnings.empty());
  CHECK(r.config.graphics.backend == RenderBackend::Vulkan);
  CHECK(r.config.graphics.present_mode == PresentMode::Fifo);
  CHECK(r.config.graphics.frames_in_flight == 2);
  CHECK_FALSE(r.config.input.keyboard_mouse.empty());
}

TEST_CASE("graphics values parse") {
  auto r = parse_config(R"(
[graphics]
backend = "null"
present_mode = "mailbox"
window_mode = "borderless"
width = 1920
height = 1080
resolution_scale = 4
aspect = "stretch"
validation = "on"
preferred_gpu = "radeon"
)");
  REQUIRE(r.ok());
  const auto& g = r.config.graphics;
  CHECK(g.backend == RenderBackend::Null);
  CHECK(g.present_mode == PresentMode::Mailbox);
  CHECK(g.window_mode == WindowMode::Borderless);
  CHECK(g.width == 1920);
  CHECK(g.height == 1080);
  CHECK(g.resolution_scale == 4);
  CHECK(g.aspect == AspectMode::Stretch);
  CHECK(g.validation == Validation::On);
  CHECK(g.preferred_gpu == "radeon");
}

TEST_CASE("syntax errors are reported with location and defaults kept") {
  auto r = parse_config("[graphics\nwidth = 3");
  CHECK_FALSE(r.ok());
  CHECK(any_contains(r.errors, "line"));
  CHECK(r.config.graphics.width == 1280);
}

TEST_CASE("type errors and bad enum values are errors; others still apply") {
  auto r = parse_config(R"(
[graphics]
width = "wide"
present_mode = "turbo"
height = 1000
)");
  CHECK(r.errors.size() == 2);
  CHECK(any_contains(r.errors, "graphics.width"));
  CHECK(any_contains(r.errors, "\"fifo\""));
  CHECK(r.config.graphics.width == 1280);
  CHECK(r.config.graphics.height == 1000);
}

TEST_CASE("out of range values clamp with a warning") {
  auto r = parse_config("[graphics]\nresolution_scale = 99\nframes_in_flight = 0\n");
  CHECK(r.ok());
  CHECK(r.config.graphics.resolution_scale == 8);
  CHECK(r.config.graphics.frames_in_flight == 1);
  CHECK(r.warnings.size() == 2);
}

TEST_CASE("unknown keys warn") {
  auto r = parse_config("colour = 1\n[graphics]\nvsinc = true\n");
  CHECK(r.ok());
  CHECK(any_contains(r.warnings, "'colour'"));
  CHECK(any_contains(r.warnings, "'graphics.vsinc'"));
}

TEST_CASE("bindings override per target; unlisted targets keep defaults") {
  auto r = parse_config(R"(
[input.keyboard_mouse]
cross = ["Return", "mouse_left"]
triangle = []
[input.gamepad]
cross = "east"
)");
  REQUIRE(r.ok());
  const auto& km = r.config.input.keyboard_mouse;
  CHECK(km.sources(Target::button(PadButton::Cross)).size() == 2);
  CHECK(km.sources(Target::button(PadButton::Triangle)).empty());
  CHECK(km.target_for(*source_from_name("Return")) == Target::button(PadButton::Cross));
  CHECK(any_contains(r.warnings, "was bound to 'start'"));             // Return moved off start
  CHECK_FALSE(km.sources(Target::button(PadButton::Square)).empty());  // default kept
  const auto& gp = r.config.input.gamepad;
  CHECK(gp.target_for(*source_from_name("east")) == Target::button(PadButton::Cross));
}

TEST_CASE("binding problems are warnings, not fatal") {
  auto r = parse_config(R"(
[input.keyboard_mouse]
cross = ["NoSuchKey", "south"]
jump = ["Space"]
[input.gamepad]
circle = 5
)");
  CHECK(any_contains(r.warnings, "unknown input 'NoSuchKey'"));
  CHECK(any_contains(r.warnings, "belongs in [input.gamepad]"));
  CHECK(any_contains(r.warnings, "unknown binding target"));
  CHECK(any_contains(r.errors, "input.gamepad.circle"));
}

TEST_CASE("dead zones parse and invalid combinations reset") {
  auto r = parse_config(R"(
[input.deadzone]
left_inner = 0.25
left_outer = 0.9
right_inner = 0.8
right_outer = 0.5
)");
  CHECK(r.config.input.left_stick.inner == doctest::Approx(0.25f));
  CHECK(r.config.input.left_stick.outer == doctest::Approx(0.9f));
  CHECK(r.config.input.right_stick.inner == doctest::Approx(StickDeadzone{}.inner));
  CHECK(any_contains(r.warnings, "outer <= inner"));
}

TEST_CASE("serialise -> parse round-trips without warnings") {
  Config c;
  c.graphics.present_mode = PresentMode::Immediate;
  c.graphics.window_mode = WindowMode::Exclusive;
  c.graphics.width = 1600;
  c.game.iso_path = "/games/disc.iso";
  c.input.invert_mouse_y = true;
  c.input.left_stick.inner = 0.3f;
  c.input.keyboard_mouse.clear(Target::button(PadButton::Triangle));
  c.input.keyboard_mouse.bind(Target::button(PadButton::Cross), *source_from_name("KP_Enter"));
  auto text = serialize_config(c);
  auto r = parse_config(text);
  INFO(text);
  CHECK(r.ok());
  CHECK(r.warnings.empty());
  CHECK(r.config.graphics.present_mode == PresentMode::Immediate);
  CHECK(r.config.graphics.window_mode == WindowMode::Exclusive);
  CHECK(r.config.graphics.width == 1600);
  CHECK(r.config.game.iso_path == "/games/disc.iso");
  CHECK(r.config.input.invert_mouse_y);
  CHECK(r.config.input.left_stick.inner == doctest::Approx(0.3f));
  CHECK(r.config.input.keyboard_mouse.entries() == c.input.keyboard_mouse.entries());
  CHECK(r.config.input.gamepad.entries() == c.input.gamepad.entries());
}

TEST_CASE("save and load via file") {
  auto dir = std::filesystem::temp_directory_path() / "lantern_test_config";
  std::filesystem::remove_all(dir);
  Config c;
  c.graphics.height = 720;
  std::string err;
  REQUIRE(save_config(c, dir / "sub" / "config.toml", &err));
  auto r = load_config(dir / "sub" / "config.toml");
  CHECK(r.ok());
  CHECK(r.config.graphics.height == 720);
  CHECK_FALSE(load_config(dir / "missing.toml").ok());
  std::filesystem::remove_all(dir);
}
