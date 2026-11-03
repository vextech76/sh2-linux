#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lantern::input {

// Virtual DualShock 2. This is the only controller model the game sees; every host device is
// mapped onto it. Button order matches the DS2 digital report bit order (LSB first).
enum class PadButton : uint8_t {
  Select,
  L3,
  R3,
  Start,
  Up,
  Right,
  Down,
  Left,
  L2,
  R2,
  L1,
  R1,
  Triangle,
  Circle,
  Cross,
  Square,
  Count
};

enum class PadAxis : uint8_t { LeftX, LeftY, RightX, RightY, Count };

inline constexpr size_t kPadButtonCount = static_cast<size_t>(PadButton::Count);
inline constexpr size_t kPadAxisCount = static_cast<size_t>(PadAxis::Count);

struct PadState {
  uint16_t buttons = 0;                                         // bit set = pressed (host convention)
  std::array<uint8_t, kPadButtonCount> pressure{};              // 0..255, DS2 pressure-sensitive values
  std::array<uint8_t, kPadAxisCount> axes{128, 128, 128, 128};  // 0..255, 128 = centre

  bool pressed(PadButton b) const { return (buttons >> static_cast<unsigned>(b)) & 1u; }
  uint8_t axis(PadAxis a) const { return axes[static_cast<size_t>(a)]; }

  // DS2 wire format: digital buttons are active-low.
  uint16_t wire_buttons() const { return static_cast<uint16_t>(~buttons); }
};

// Map a float stick value in [-1, 1] to DS2 byte range (0 = left/up, 255 = right/down).
uint8_t axis_to_byte(float v);

std::string_view pad_button_name(PadButton b);
std::optional<PadButton> pad_button_from_name(std::string_view name);

}  // namespace lantern::input
