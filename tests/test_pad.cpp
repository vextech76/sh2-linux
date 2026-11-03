#include <doctest/doctest.h>

#include "input/pad.h"

using namespace lantern::input;

TEST_CASE("axis byte mapping matches DS2 range") {
  CHECK(axis_to_byte(0.0f) == 128);
  CHECK(axis_to_byte(-1.0f) == 0);
  CHECK(axis_to_byte(1.0f) == 255);
  CHECK(axis_to_byte(-5.0f) == 0);
  CHECK(axis_to_byte(5.0f) == 255);
}

TEST_CASE("wire format is active-low") {
  PadState s;
  CHECK(s.wire_buttons() == 0xFFFF);
  s.buttons = 1u << static_cast<unsigned>(PadButton::Cross);
  CHECK(s.pressed(PadButton::Cross));
  CHECK(s.wire_buttons() == static_cast<uint16_t>(~(1u << 14)));
}

TEST_CASE("pad button names round-trip") {
  for (size_t i = 0; i < kPadButtonCount; ++i) {
    auto b = static_cast<PadButton>(i);
    CHECK(pad_button_from_name(pad_button_name(b)) == b);
  }
  CHECK_FALSE(pad_button_from_name("nope").has_value());
}
