#include <doctest/doctest.h>

#include "input/binding.h"

using namespace lantern::input;

TEST_CASE("source names parse case-insensitively and format canonically") {
  auto w = source_from_name("w");
  REQUIRE(w);
  CHECK(w->kind == SourceKind::Key);
  CHECK(w->code == 26);  // HID usage for W
  CHECK(source_name(*w) == "W");
  CHECK(source_name(*source_from_name("leftshift")) == "LeftShift");
  CHECK(source_name(*source_from_name("F12")) == "F12");
  CHECK(source_name(*source_from_name("KP_0")) == "KP_0");
  CHECK(source_from_name("Key100")->code == 100);
  CHECK_FALSE(source_from_name("Key99999"));
  CHECK_FALSE(source_from_name("NotAKey"));
}

TEST_CASE("mouse and gamepad sources") {
  auto m = source_from_name("mouse_right");
  REQUIRE(m);
  CHECK(m->kind == SourceKind::MouseButton);
  CHECK(m->code == 3);
  auto mx = source_from_name("mouse_x-");
  REQUIRE(mx);
  CHECK(mx->kind == SourceKind::MouseAxis);
  CHECK(mx->direction == -1);
  auto ly = source_from_name("left_y+");
  REQUIRE(ly);
  CHECK(ly->kind == SourceKind::GamepadAxis);
  CHECK(ly->direction == 1);
  CHECK(is_gamepad_source(*ly));
  CHECK_FALSE(is_gamepad_source(*mx));
  CHECK(source_name(*source_from_name("left_trigger")) == "left_trigger");
  CHECK(source_name(*source_from_name("south")) == "south");
}

TEST_CASE("every target name round-trips") {
  for (const auto& t : all_targets()) {
    auto parsed = target_from_name(target_name(t));
    REQUIRE(parsed);
    CHECK(*parsed == t);
  }
  CHECK(all_targets().size() == 16 + 8 + 3);
}

TEST_CASE("binding a source moves it away from another target and reports the conflict") {
  BindingTable t;
  auto q = *source_from_name("Q");
  t.bind(Target::button(PadButton::Circle), q);
  auto conflicts = t.bind(Target::button(PadButton::Cross), q);
  REQUIRE(conflicts.size() == 1);
  CHECK(conflicts[0].target == Target::button(PadButton::Circle));
  CHECK(t.sources(Target::button(PadButton::Circle)).empty());
  CHECK(t.target_for(q) == Target::button(PadButton::Cross));
}

TEST_CASE("replace mode drops previous sources; add mode keeps them") {
  BindingTable t;
  auto a = *source_from_name("A"), b = *source_from_name("B");
  auto target = Target::button(PadButton::Square);
  t.bind(target, a);
  t.bind(target, b);
  CHECK(t.sources(target).size() == 2);
  t.bind(target, a);  // duplicate add is a no-op
  CHECK(t.sources(target).size() == 2);
  t.bind(target, b, BindingTable::Mode::Replace);
  REQUIRE(t.sources(target).size() == 1);
  CHECK(t.sources(target)[0] == b);
  t.unbind(target, b);
  CHECK(t.empty());
}

TEST_CASE("default bindings cover every pad button on both profiles") {
  auto km = default_keyboard_mouse_bindings();
  auto gp = default_gamepad_bindings();
  for (size_t i = 0; i < kPadButtonCount; ++i) {
    auto t = Target::button(static_cast<PadButton>(i));
    CHECK_MESSAGE(!km.sources(t).empty(), target_name(t));
    CHECK_MESSAGE(!gp.sources(t).empty(), target_name(t));
  }
  for (const auto& [t, sources] : gp.entries())
    for (const auto& s : sources) CHECK(is_gamepad_source(s));
  for (const auto& [t, sources] : km.entries())
    for (const auto& s : sources) CHECK_FALSE(is_gamepad_source(s));
}
