#include "input/pad.h"

#include <algorithm>
#include <cmath>

namespace lantern::input {
namespace {
constexpr std::array<std::string_view, kPadButtonCount> kNames = {
    "select", "l3", "r3", "start", "dpad_up",  "dpad_right", "dpad_down", "dpad_left",
    "l2",     "r2", "l1", "r1",    "triangle", "circle",     "cross",     "square"};
}

uint8_t axis_to_byte(float v) {
  v = std::clamp(v, -1.0f, 1.0f);
  // -1 -> 0, 0 -> 128, +1 -> 255
  float scaled = v < 0.0f ? 128.0f + v * 128.0f : 128.0f + v * 127.0f;
  return static_cast<uint8_t>(std::lround(scaled));
}

std::string_view pad_button_name(PadButton b) {
  auto i = static_cast<size_t>(b);
  return i < kNames.size() ? kNames[i] : "invalid";
}

std::optional<PadButton> pad_button_from_name(std::string_view name) {
  for (size_t i = 0; i < kNames.size(); ++i)
    if (kNames[i] == name) return static_cast<PadButton>(i);
  return std::nullopt;
}

}  // namespace lantern::input
