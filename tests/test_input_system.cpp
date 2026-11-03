#include <doctest/doctest.h>

#include "input/input_system.h"

using namespace lantern::input;

namespace {
uint16_t key(const char* name) { return source_from_name(name)->code; }
}  // namespace

TEST_CASE("keyboard drives pad buttons and sticks") {
  InputSystem in;
  in.on_key(key("Space"), true);
  in.on_key(key("W"), true);
  in.on_key(key("D"), true);
  auto s = in.sample();
  CHECK(s.pressed(PadButton::Cross));
  CHECK(s.pressure[static_cast<size_t>(PadButton::Cross)] == 255);
  CHECK(s.axis(PadAxis::LeftY) == 0);    // up
  CHECK(s.axis(PadAxis::LeftX) == 255);  // right
  CHECK(s.axis(PadAxis::RightX) == 128);

  in.on_key(key("Space"), false);
  in.on_key(key("W"), false);
  in.on_key(key("S"), true);
  s = in.sample();
  CHECK_FALSE(s.pressed(PadButton::Cross));
  CHECK(s.pressure[static_cast<size_t>(PadButton::Cross)] == 0);
  CHECK(s.axis(PadAxis::LeftY) == 255);
}

TEST_CASE("opposing directions cancel") {
  InputSystem in;
  in.on_key(key("A"), true);
  in.on_key(key("D"), true);
  CHECK(in.sample().axis(PadAxis::LeftX) == 128);
}

TEST_CASE("gamepad sticks go through the radial dead zone") {
  InputSystem in;
  in.on_gamepad_added(7, "pad");
  in.on_gamepad_axis(7, GamepadAxis::LeftX, 0.1f);  // inside default 0.15 dead zone
  CHECK(in.sample().axis(PadAxis::LeftX) == 128);
  in.on_gamepad_axis(7, GamepadAxis::LeftX, -1.0f);
  CHECK(in.sample().axis(PadAxis::LeftX) == 0);
}

TEST_CASE("analog trigger maps to pressure and threshold") {
  InputSystem in;
  in.on_gamepad_added(1, "pad");
  in.on_gamepad_axis(1, GamepadAxis::RightTrigger, 0.3f);
  auto s = in.sample();
  CHECK_FALSE(s.pressed(PadButton::R2));
  in.on_gamepad_axis(1, GamepadAxis::RightTrigger, 1.0f);
  s = in.sample();
  CHECK(s.pressed(PadButton::R2));
  CHECK(s.pressure[static_cast<size_t>(PadButton::R2)] == 255);
}

TEST_CASE("hot-plug: first connected pad is active, removal falls back to the next") {
  InputSystem in;
  CHECK_FALSE(in.active_gamepad());
  in.on_gamepad_added(10, "first");
  in.on_gamepad_added(11, "second");
  in.on_gamepad_added(10, "duplicate");  // ignored
  CHECK(in.gamepads().size() == 2);
  CHECK(in.active_gamepad() == 10u);

  in.on_gamepad_button(10, GamepadButton::South, true);
  CHECK(in.sample().pressed(PadButton::Cross));
  in.on_gamepad_button(11, GamepadButton::East, true);  // inactive pad ignored
  CHECK_FALSE(in.sample().pressed(PadButton::Circle));

  in.on_gamepad_removed(10);
  CHECK(in.active_gamepad() == 11u);
  auto s = in.sample();
  CHECK_FALSE(s.pressed(PadButton::Cross));  // state of the unplugged pad is gone
  CHECK(s.axis(PadAxis::LeftX) == 128);

  in.on_gamepad_removed(11);
  CHECK_FALSE(in.active_gamepad());
  in.on_gamepad_removed(99);  // unknown id is harmless
}

TEST_CASE("keyboard and gamepad combine") {
  InputSystem in;
  in.on_gamepad_added(1, "pad");
  in.on_gamepad_button(1, GamepadButton::North, true);
  in.on_key(key("Space"), true);
  auto s = in.sample();
  CHECK(s.pressed(PadButton::Triangle));
  CHECK(s.pressed(PadButton::Cross));
}

TEST_CASE("mouse motion is per-frame and bindable") {
  InputSettings settings;
  settings.keyboard_mouse.bind(Target::axis(PadAxis::RightX, +1), *source_from_name("mouse_x+"));
  settings.keyboard_mouse.bind(Target::axis(PadAxis::RightX, -1), *source_from_name("mouse_x-"));
  settings.mouse_sensitivity = 0.1f;
  InputSystem in(settings);
  in.on_mouse_motion(5.0f, 0.0f);
  CHECK(in.sample().axis(PadAxis::RightX) == lantern::input::axis_to_byte(0.5f));
  CHECK(in.sample().axis(PadAxis::RightX) == 128);  // consumed
  in.on_mouse_motion(-100.0f, 0.0f);
  CHECK(in.sample().axis(PadAxis::RightX) == 0);
}

TEST_CASE("host actions fire once per press edge") {
  InputSystem in;
  in.on_key(key("F11"), true);
  in.on_key(key("F11"), true);  // repeat while held
  auto actions = in.take_host_actions();
  REQUIRE(actions.size() == 1);
  CHECK(actions[0] == HostAction::ToggleFullscreen);
  CHECK(in.take_host_actions().empty());
  in.on_key(key("F11"), false);
  in.on_key(key("F11"), true);
  CHECK(in.take_host_actions().size() == 1);
}

TEST_CASE("release_all clears held input") {
  InputSystem in;
  in.on_gamepad_added(1, "pad");
  in.on_key(key("Space"), true);
  in.on_gamepad_axis(1, GamepadAxis::LeftY, 1.0f);
  in.release_all();
  auto s = in.sample();
  CHECK(s.buttons == 0);
  CHECK(s.axis(PadAxis::LeftY) == 128);
}

TEST_CASE("capture swallows the next input for rebinding") {
  InputSystem in;
  in.begin_capture();
  CHECK(in.capturing());
  in.on_key(key("Space"), true);
  auto c = in.take_captured();
  REQUIRE(c);
  CHECK(c->kind == SourceKind::Key);
  CHECK_FALSE(in.capturing());
  CHECK_FALSE(in.sample().pressed(PadButton::Cross));  // swallowed

  in.on_gamepad_added(1, "pad");
  in.begin_capture();
  in.on_gamepad_axis(1, GamepadAxis::LeftX, 0.2f);  // below capture threshold
  CHECK_FALSE(in.take_captured());
  in.on_gamepad_axis(1, GamepadAxis::LeftX, -0.9f);
  c = in.take_captured();
  REQUIRE(c);
  CHECK(source_name(*c) == "left_x-");

  in.begin_capture();
  in.on_gamepad_axis(1, GamepadAxis::LeftTrigger, 1.0f);
  c = in.take_captured();
  REQUIRE(c);
  CHECK(source_name(*c) == "left_trigger");
}

TEST_CASE("rebinding at runtime takes effect") {
  InputSystem in;
  auto& km = in.mutable_settings().keyboard_mouse;
  km.bind(Target::button(PadButton::Cross), *source_from_name("Q"), BindingTable::Mode::Replace);
  in.on_key(key("Q"), true);
  auto s = in.sample();
  CHECK(s.pressed(PadButton::Cross));
  CHECK_FALSE(s.pressed(PadButton::Circle));  // Q moved off circle
  in.on_key(key("Q"), false);
  in.on_key(key("Space"), true);
  CHECK_FALSE(in.sample().pressed(PadButton::Cross));  // replaced
}
