#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "input/pad.h"

namespace lantern::input {

// Host gamepad layout (values intentionally equal SDL3's SDL_GamepadButton / SDL_GamepadAxis;
// the SDL adapter static_asserts this). Positional naming: south = bottom face button.
enum class GamepadButton : uint8_t {
  South,
  East,
  West,
  North,
  Back,
  Guide,
  Start,
  LeftStick,
  RightStick,
  LeftShoulder,
  RightShoulder,
  DpadUp,
  DpadDown,
  DpadLeft,
  DpadRight,
  Misc1,
  RightPaddle1,
  LeftPaddle1,
  RightPaddle2,
  LeftPaddle2,
  Touchpad,
  Count
};
enum class GamepadAxis : uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

// Mouse buttons use SDL numbering (1 = left, 2 = middle, 3 = right, 4/5 = side).
enum class MouseAxis : uint8_t { X, Y };

enum class SourceKind : uint8_t { Key, MouseButton, MouseAxis, GamepadButton, GamepadAxis };

// A physical input. `code` is a USB HID usage for keys (== SDL scancode), otherwise the enum
// value above. `direction` is +1/-1 for axis halves, 0 for buttons and triggers.
struct Source {
  SourceKind kind{};
  uint16_t code = 0;
  int8_t direction = 0;
  auto operator<=>(const Source&) const = default;
};

enum class HostAction : uint8_t { ToggleFullscreen, ToggleMouseCapture, Quit, Count };

enum class TargetKind : uint8_t { PadButton, PadAxis, Host };

// A logical destination on the virtual pad or a host-side action.
struct Target {
  TargetKind kind{};
  uint8_t code = 0;
  int8_t direction = 0;  // for PadAxis: -1 = up/left, +1 = down/right
  auto operator<=>(const Target&) const = default;

  static Target button(PadButton b) { return {TargetKind::PadButton, static_cast<uint8_t>(b), 0}; }
  static Target axis(PadAxis a, int8_t dir) { return {TargetKind::PadAxis, static_cast<uint8_t>(a), dir}; }
  static Target host(HostAction h) { return {TargetKind::Host, static_cast<uint8_t>(h), 0}; }
};

std::string target_name(const Target& t);
std::optional<Target> target_from_name(std::string_view name);
std::vector<Target> all_targets();

// Names are case-insensitive on parse; canonical forms are produced on format.
std::string source_name(const Source& s);
std::optional<Source> source_from_name(std::string_view name);

// Which device family a source belongs to (keyboard+mouse vs. gamepad profile).
bool is_gamepad_source(const Source& s);

struct BindConflict {
  Target target;  // the other target that already used the source
  Source source;
};

// Target -> list of sources. A source may be bound to only one target per table, so a single
// key press never drives two pad inputs by accident; bind() reports and resolves conflicts.
class BindingTable {
 public:
  enum class Mode { Add, Replace };

  // Bind `source` to `target`. Any other target using `source` loses it; those are returned.
  std::vector<BindConflict> bind(const Target& target, const Source& source, Mode mode = Mode::Add);
  void unbind(const Target& target, const Source& source);
  void clear(const Target& target);
  void clear_all() { map_.clear(); }

  const std::vector<Source>& sources(const Target& target) const;
  std::optional<Target> target_for(const Source& source) const;
  const std::map<Target, std::vector<Source>>& entries() const { return map_; }
  bool empty() const { return map_.empty(); }

 private:
  std::map<Target, std::vector<Source>> map_;
};

BindingTable default_keyboard_mouse_bindings();
BindingTable default_gamepad_bindings();

}  // namespace lantern::input
