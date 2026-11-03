#include "input/input_system.h"

#include <algorithm>
#include <cmath>

namespace lantern::input {
namespace {
constexpr float kCaptureAxisThreshold = 0.6f;
constexpr size_t kAxisIdx(GamepadAxis a) { return static_cast<size_t>(a); }
}  // namespace

InputSystem::InputSystem(InputSettings settings) : settings_(std::move(settings)) {}

void InputSystem::set_settings(InputSettings settings) { settings_ = std::move(settings); }

bool InputSystem::try_capture(const Source& s) {
  if (!capturing_) return false;
  captured_ = s;
  capturing_ = false;
  return true;
}

void InputSystem::edge(const BindingTable& table, const Source& s, bool down) {
  if (!down) return;
  if (auto t = table.target_for(s); t && t->kind == TargetKind::Host)
    host_actions_.push_back(static_cast<HostAction>(t->code));
}

void InputSystem::on_key(uint16_t scancode, bool down) {
  if (scancode >= keys_.size()) return;
  Source s{SourceKind::Key, scancode, 0};
  if (down && !keys_[scancode] && try_capture(s)) return;
  if (down && !keys_[scancode]) edge(settings_.keyboard_mouse, s, true);
  keys_[scancode] = down;
}

void InputSystem::on_mouse_button(uint8_t button, bool down) {
  if (button >= mouse_buttons_.size()) return;
  Source s{SourceKind::MouseButton, button, 0};
  if (down && !mouse_buttons_[button] && try_capture(s)) return;
  if (down && !mouse_buttons_[button]) edge(settings_.keyboard_mouse, s, true);
  mouse_buttons_[button] = down;
}

void InputSystem::on_mouse_motion(float dx, float dy) {
  mouse_dx_ += dx;
  mouse_dy_ += dy;
}

void InputSystem::on_gamepad_added(uint32_t id, std::string name) {
  for (const auto& g : gamepads_)
    if (g.id == id) return;
  gamepads_.push_back({id, std::move(name)});
  gamepad_states_.emplace_back();
}

void InputSystem::on_gamepad_removed(uint32_t id) {
  for (size_t i = 0; i < gamepads_.size(); ++i) {
    if (gamepads_[i].id == id) {
      gamepads_.erase(gamepads_.begin() + static_cast<std::ptrdiff_t>(i));
      gamepad_states_.erase(gamepad_states_.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
  }
}

void InputSystem::on_gamepad_button(uint32_t id, GamepadButton button, bool down) {
  if (gamepads_.empty() || gamepads_.front().id != id) return;  // only the active pad drives input
  auto idx = static_cast<size_t>(button);
  if (idx >= static_cast<size_t>(GamepadButton::Count)) return;
  auto& st = gamepad_states_.front();
  Source s{SourceKind::GamepadButton, static_cast<uint16_t>(idx), 0};
  if (down && !st.buttons[idx] && try_capture(s)) return;
  if (down && !st.buttons[idx]) edge(settings_.gamepad, s, true);
  st.buttons[idx] = down;
}

void InputSystem::on_gamepad_axis(uint32_t id, GamepadAxis axis, float value) {
  if (gamepads_.empty() || gamepads_.front().id != id) return;
  auto idx = static_cast<size_t>(axis);
  if (idx >= static_cast<size_t>(GamepadAxis::Count)) return;
  const bool trigger = axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger;
  value = trigger ? std::clamp(value, 0.0f, 1.0f) : std::clamp(value, -1.0f, 1.0f);
  auto& st = gamepad_states_.front();
  if (capturing_ && std::fabs(value) >= kCaptureAxisThreshold && std::fabs(st.axes[idx]) < kCaptureAxisThreshold) {
    int8_t dir = trigger ? int8_t{0} : static_cast<int8_t>(value > 0 ? 1 : -1);
    if (try_capture({SourceKind::GamepadAxis, static_cast<uint16_t>(idx), dir})) {
      st.axes[idx] = value;
      return;
    }
  }
  st.axes[idx] = value;
}

void InputSystem::release_all() {
  keys_.reset();
  mouse_buttons_.reset();
  mouse_dx_ = mouse_dy_ = 0.0f;
  for (auto& st : gamepad_states_) st = GamepadState{};
}

float InputSystem::source_amount(const Source& s, const GamepadState* pad, const Vec2& left, const Vec2& right,
                                 float lt, float rt) const {
  switch (s.kind) {
    case SourceKind::Key: return s.code < keys_.size() && keys_[s.code] ? 1.0f : 0.0f;
    case SourceKind::MouseButton: return s.code < mouse_buttons_.size() && mouse_buttons_[s.code] ? 1.0f : 0.0f;
    case SourceKind::MouseAxis: {
      float d = s.code == 0 ? mouse_dx_ : mouse_dy_ * (settings_.invert_mouse_y ? -1.0f : 1.0f);
      return std::clamp(d * static_cast<float>(s.direction) * settings_.mouse_sensitivity, 0.0f, 1.0f);
    }
    case SourceKind::GamepadButton:
      if (!pad || s.code >= pad->buttons.size()) return 0.0f;
      return pad->buttons[s.code] ? 1.0f : 0.0f;
    case SourceKind::GamepadAxis: {
      if (!pad) return 0.0f;
      float v = 0.0f;
      switch (static_cast<GamepadAxis>(s.code)) {
        case GamepadAxis::LeftX: v = left.x; break;
        case GamepadAxis::LeftY: v = left.y; break;
        case GamepadAxis::RightX: v = right.x; break;
        case GamepadAxis::RightY: v = right.y; break;
        case GamepadAxis::LeftTrigger: return lt;
        case GamepadAxis::RightTrigger: return rt;
        default: return 0.0f;
      }
      return std::max(0.0f, v * static_cast<float>(s.direction));
    }
  }
  return 0.0f;
}

PadState InputSystem::sample() {
  const GamepadState* pad = gamepad_states_.empty() ? nullptr : &gamepad_states_.front();
  Vec2 left{}, right{};
  float lt = 0.0f, rt = 0.0f;
  if (pad) {
    left = apply_radial_deadzone({pad->axes[kAxisIdx(GamepadAxis::LeftX)], pad->axes[kAxisIdx(GamepadAxis::LeftY)]},
                                 settings_.left_stick);
    right = apply_radial_deadzone({pad->axes[kAxisIdx(GamepadAxis::RightX)], pad->axes[kAxisIdx(GamepadAxis::RightY)]},
                                  settings_.right_stick);
    lt = apply_trigger_deadzone(pad->axes[kAxisIdx(GamepadAxis::LeftTrigger)], settings_.triggers);
    rt = apply_trigger_deadzone(pad->axes[kAxisIdx(GamepadAxis::RightTrigger)], settings_.triggers);
  }

  auto amount = [&](const Target& t) {
    float a = 0.0f;
    for (const auto& s : settings_.keyboard_mouse.sources(t))
      a = std::max(a, source_amount(s, pad, left, right, lt, rt));
    for (const auto& s : settings_.gamepad.sources(t)) a = std::max(a, source_amount(s, pad, left, right, lt, rt));
    return a;
  };

  PadState out;
  for (size_t i = 0; i < kPadButtonCount; ++i) {
    float a = amount(Target::button(static_cast<PadButton>(i)));
    if (a >= settings_.button_threshold) {
      out.buttons = static_cast<uint16_t>(out.buttons | (1u << i));
      out.pressure[i] = static_cast<uint8_t>(std::clamp(std::lround(a * 255.0f), 1L, 255L));
    }
  }
  for (size_t i = 0; i < kPadAxisCount; ++i) {
    auto axis = static_cast<PadAxis>(i);
    float v = amount(Target::axis(axis, +1)) - amount(Target::axis(axis, -1));
    out.axes[i] = axis_to_byte(v);
  }
  mouse_dx_ = mouse_dy_ = 0.0f;
  return out;
}

std::vector<HostAction> InputSystem::take_host_actions() {
  std::vector<HostAction> out;
  out.swap(host_actions_);
  return out;
}

std::optional<uint32_t> InputSystem::active_gamepad() const {
  if (gamepads_.empty()) return std::nullopt;
  return gamepads_.front().id;
}

void InputSystem::begin_capture() {
  capturing_ = true;
  captured_.reset();
}

void InputSystem::cancel_capture() { capturing_ = false; }

std::optional<Source> InputSystem::take_captured() {
  auto c = captured_;
  captured_.reset();
  return c;
}

}  // namespace lantern::input
