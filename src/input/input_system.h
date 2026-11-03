#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "input/binding.h"
#include "input/deadzone.h"
#include "input/pad.h"

namespace lantern::input {

struct InputSettings {
  BindingTable keyboard_mouse = default_keyboard_mouse_bindings();
  BindingTable gamepad = default_gamepad_bindings();
  StickDeadzone left_stick{};
  StickDeadzone right_stick{};
  TriggerDeadzone triggers{};
  float button_threshold = 0.5f;    // analog amount at which a bound pad button reads pressed
  float mouse_sensitivity = 0.05f;  // stick deflection per pixel of motion within one frame
  bool invert_mouse_y = false;
};

struct GamepadInfo {
  uint32_t id = 0;
  std::string name;
};

// Device-agnostic input aggregator. Platform code feeds it events; the emulated pad driver
// samples a PadState once per game frame. No SDL dependency, so it is fully unit-testable.
class InputSystem {
 public:
  explicit InputSystem(InputSettings settings = {});

  void set_settings(InputSettings settings);
  const InputSettings& settings() const { return settings_; }
  InputSettings& mutable_settings() { return settings_; }

  void on_key(uint16_t scancode, bool down);
  void on_mouse_button(uint8_t button, bool down);
  void on_mouse_motion(float dx, float dy);
  void on_gamepad_added(uint32_t id, std::string name);
  void on_gamepad_removed(uint32_t id);
  void on_gamepad_button(uint32_t id, GamepadButton button, bool down);
  // Sticks in [-1, 1] (+y = down), triggers in [0, 1].
  void on_gamepad_axis(uint32_t id, GamepadAxis axis, float value);
  // Release everything (window focus lost), so no input sticks down.
  void release_all();

  // Compute the virtual pad for this frame and consume accumulated mouse motion.
  PadState sample();
  std::vector<HostAction> take_host_actions();

  std::optional<uint32_t> active_gamepad() const;
  const std::vector<GamepadInfo>& gamepads() const { return gamepads_; }

  // Rebinding: the next distinct physical input is captured instead of being processed.
  void begin_capture();
  void cancel_capture();
  bool capturing() const { return capturing_; }
  std::optional<Source> take_captured();

 private:
  struct GamepadState {
    std::bitset<static_cast<size_t>(GamepadButton::Count)> buttons;
    std::array<float, static_cast<size_t>(GamepadAxis::Count)> axes{};
  };

  float source_amount(const Source& s, const GamepadState* pad, const Vec2& left, const Vec2& right, float lt,
                      float rt) const;
  bool try_capture(const Source& s);
  void edge(const BindingTable& table, const Source& s, bool down);

  InputSettings settings_;
  std::bitset<512> keys_;
  std::bitset<8> mouse_buttons_;
  float mouse_dx_ = 0.0f;
  float mouse_dy_ = 0.0f;
  std::vector<GamepadInfo> gamepads_;         // in connection order; front = active
  std::vector<GamepadState> gamepad_states_;  // parallel to gamepads_
  std::vector<HostAction> host_actions_;
  bool capturing_ = false;
  std::optional<Source> captured_;
};

}  // namespace lantern::input
