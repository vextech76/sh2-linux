#include "input/binding.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace lantern::input {
namespace {

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// USB HID usage page 0x07 codes (identical to SDL scancodes).
const std::vector<std::pair<uint16_t, std::string>>& key_table() {
  static const auto table = [] {
    std::vector<std::pair<uint16_t, std::string>> t;
    for (int i = 0; i < 26; ++i)
      t.emplace_back(static_cast<uint16_t>(4 + i), std::string(1, static_cast<char>('A' + i)));
    for (int i = 1; i <= 9; ++i)
      t.emplace_back(static_cast<uint16_t>(29 + i), std::string(1, static_cast<char>('0' + i)));
    t.emplace_back(39, "0");
    const std::pair<uint16_t, const char*> named[] = {
        {40, "Return"},    {41, "Escape"},     {42, "Backspace"},   {43, "Tab"},          {44, "Space"},
        {45, "Minus"},     {46, "Equals"},     {47, "LeftBracket"}, {48, "RightBracket"}, {49, "Backslash"},
        {51, "Semicolon"}, {52, "Apostrophe"}, {53, "Grave"},       {54, "Comma"},        {55, "Period"},
        {56, "Slash"},     {57, "CapsLock"},   {70, "PrintScreen"}, {71, "ScrollLock"},   {72, "Pause"},
        {73, "Insert"},    {74, "Home"},       {75, "PageUp"},      {76, "Delete"},       {77, "End"},
        {78, "PageDown"},  {79, "Right"},      {80, "Left"},        {81, "Down"},         {82, "Up"},
        {83, "NumLock"},   {84, "KP_Divide"},  {85, "KP_Multiply"}, {86, "KP_Minus"},     {87, "KP_Plus"},
        {88, "KP_Enter"},  {99, "KP_Period"},  {224, "LeftCtrl"},   {225, "LeftShift"},   {226, "LeftAlt"},
        {227, "LeftGui"},  {228, "RightCtrl"}, {229, "RightShift"}, {230, "RightAlt"},    {231, "RightGui"}};
    for (auto& [c, n] : named) t.emplace_back(c, n);
    for (int i = 0; i < 12; ++i) t.emplace_back(static_cast<uint16_t>(58 + i), "F" + std::to_string(i + 1));
    for (int i = 1; i <= 9; ++i) t.emplace_back(static_cast<uint16_t>(88 + i), "KP_" + std::to_string(i));
    t.emplace_back(98, "KP_0");
    return t;
  }();
  return table;
}

constexpr std::array<std::string_view, static_cast<size_t>(GamepadButton::Count)> kGamepadButtons = {
    "south",      "east",        "west",          "north",          "back",          "guide",        "start",
    "left_stick", "right_stick", "left_shoulder", "right_shoulder", "dpad_up",       "dpad_down",    "dpad_left",
    "dpad_right", "misc1",       "right_paddle1", "left_paddle1",   "right_paddle2", "left_paddle2", "touchpad"};

constexpr std::array<std::string_view, 4> kGamepadSticks = {"left_x", "left_y", "right_x", "right_y"};

constexpr std::array<std::string_view, 5> kMouseButtons = {"mouse_left", "mouse_middle", "mouse_right", "mouse_x1",
                                                           "mouse_x2"};

constexpr std::array<std::string_view, static_cast<size_t>(HostAction::Count)> kHostActions = {
    "toggle_fullscreen", "toggle_mouse_capture", "quit"};

struct StickTargetName {
  std::string_view name;
  PadAxis axis;
  int8_t dir;
};
constexpr StickTargetName kStickTargets[] = {
    {"left_stick_up", PadAxis::LeftY, -1},     {"left_stick_down", PadAxis::LeftY, +1},
    {"left_stick_left", PadAxis::LeftX, -1},   {"left_stick_right", PadAxis::LeftX, +1},
    {"right_stick_up", PadAxis::RightY, -1},   {"right_stick_down", PadAxis::RightY, +1},
    {"right_stick_left", PadAxis::RightX, -1}, {"right_stick_right", PadAxis::RightX, +1}};

}  // namespace

std::string target_name(const Target& t) {
  switch (t.kind) {
    case TargetKind::PadButton: return std::string(pad_button_name(static_cast<PadButton>(t.code)));
    case TargetKind::PadAxis:
      for (const auto& s : kStickTargets)
        if (static_cast<uint8_t>(s.axis) == t.code && s.dir == t.direction) return std::string(s.name);
      return "invalid";
    case TargetKind::Host: return t.code < kHostActions.size() ? std::string(kHostActions[t.code]) : "invalid";
  }
  return "invalid";
}

std::optional<Target> target_from_name(std::string_view name) {
  for (size_t i = 0; i < kPadButtonCount; ++i)
    if (iequals(name, pad_button_name(static_cast<PadButton>(i)))) return Target::button(static_cast<PadButton>(i));
  for (const auto& s : kStickTargets)
    if (iequals(name, s.name)) return Target::axis(s.axis, s.dir);
  for (size_t i = 0; i < kHostActions.size(); ++i)
    if (iequals(name, kHostActions[i])) return Target::host(static_cast<HostAction>(i));
  return std::nullopt;
}

std::vector<Target> all_targets() {
  std::vector<Target> out;
  for (size_t i = 0; i < kPadButtonCount; ++i) out.push_back(Target::button(static_cast<PadButton>(i)));
  for (const auto& s : kStickTargets) out.push_back(Target::axis(s.axis, s.dir));
  for (size_t i = 0; i < kHostActions.size(); ++i) out.push_back(Target::host(static_cast<HostAction>(i)));
  return out;
}

std::string source_name(const Source& s) {
  switch (s.kind) {
    case SourceKind::Key:
      for (const auto& [code, name] : key_table())
        if (code == s.code) return name;
      return "Key" + std::to_string(s.code);
    case SourceKind::MouseButton:
      if (s.code >= 1 && s.code <= kMouseButtons.size()) return std::string(kMouseButtons[s.code - 1]);
      return "invalid";
    case SourceKind::MouseAxis: return std::string(s.code == 0 ? "mouse_x" : "mouse_y") + (s.direction < 0 ? "-" : "+");
    case SourceKind::GamepadButton:
      return s.code < kGamepadButtons.size() ? std::string(kGamepadButtons[s.code]) : "invalid";
    case SourceKind::GamepadAxis:
      if (s.code == static_cast<uint16_t>(GamepadAxis::LeftTrigger)) return "left_trigger";
      if (s.code == static_cast<uint16_t>(GamepadAxis::RightTrigger)) return "right_trigger";
      if (s.code < kGamepadSticks.size()) return std::string(kGamepadSticks[s.code]) + (s.direction < 0 ? "-" : "+");
      return "invalid";
  }
  return "invalid";
}

std::optional<Source> source_from_name(std::string_view name) {
  for (const auto& [code, n] : key_table())
    if (iequals(name, n)) return Source{SourceKind::Key, code, 0};
  if (name.size() > 3 && iequals(name.substr(0, 3), "key")) {
    int v = 0;
    for (char c : name.substr(3)) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
      v = v * 10 + (c - '0');
      if (v > 511) return std::nullopt;
    }
    return Source{SourceKind::Key, static_cast<uint16_t>(v), 0};
  }
  for (size_t i = 0; i < kMouseButtons.size(); ++i)
    if (iequals(name, kMouseButtons[i])) return Source{SourceKind::MouseButton, static_cast<uint16_t>(i + 1), 0};
  for (size_t i = 0; i < kGamepadButtons.size(); ++i)
    if (iequals(name, kGamepadButtons[i])) return Source{SourceKind::GamepadButton, static_cast<uint16_t>(i), 0};
  if (iequals(name, "left_trigger"))
    return Source{SourceKind::GamepadAxis, static_cast<uint16_t>(GamepadAxis::LeftTrigger), 0};
  if (iequals(name, "right_trigger"))
    return Source{SourceKind::GamepadAxis, static_cast<uint16_t>(GamepadAxis::RightTrigger), 0};

  if (name.size() >= 2 && (name.back() == '+' || name.back() == '-')) {
    int8_t dir = name.back() == '+' ? 1 : -1;
    auto base = name.substr(0, name.size() - 1);
    if (iequals(base, "mouse_x")) return Source{SourceKind::MouseAxis, 0, dir};
    if (iequals(base, "mouse_y")) return Source{SourceKind::MouseAxis, 1, dir};
    for (size_t i = 0; i < kGamepadSticks.size(); ++i)
      if (iequals(base, kGamepadSticks[i])) return Source{SourceKind::GamepadAxis, static_cast<uint16_t>(i), dir};
  }
  return std::nullopt;
}

bool is_gamepad_source(const Source& s) {
  return s.kind == SourceKind::GamepadButton || s.kind == SourceKind::GamepadAxis;
}

std::vector<BindConflict> BindingTable::bind(const Target& target, const Source& source, Mode mode) {
  std::vector<BindConflict> conflicts;
  if (mode == Mode::Replace) map_.erase(target);
  for (auto it = map_.begin(); it != map_.end();) {
    auto& v = it->second;
    auto found = std::find(v.begin(), v.end(), source);
    if (found != v.end() && it->first != target) {
      conflicts.push_back({it->first, source});
      v.erase(found);
    }
    it = v.empty() && it->first != target ? map_.erase(it) : std::next(it);
  }
  auto& list = map_[target];
  if (std::find(list.begin(), list.end(), source) == list.end()) list.push_back(source);
  return conflicts;
}

void BindingTable::unbind(const Target& target, const Source& source) {
  auto it = map_.find(target);
  if (it == map_.end()) return;
  std::erase(it->second, source);
  if (it->second.empty()) map_.erase(it);
}

void BindingTable::clear(const Target& target) { map_.erase(target); }

const std::vector<Source>& BindingTable::sources(const Target& target) const {
  static const std::vector<Source> kEmpty;
  auto it = map_.find(target);
  return it == map_.end() ? kEmpty : it->second;
}

std::optional<Target> BindingTable::target_for(const Source& source) const {
  for (const auto& [t, v] : map_)
    if (std::find(v.begin(), v.end(), source) != v.end()) return t;
  return std::nullopt;
}

namespace {
void add(BindingTable& t, std::string_view target, std::initializer_list<std::string_view> sources) {
  auto tg = target_from_name(target);
  for (auto s : sources) t.bind(*tg, *source_from_name(s));
}
}  // namespace

BindingTable default_keyboard_mouse_bindings() {
  BindingTable t;
  add(t, "cross", {"Space", "mouse_left"});
  add(t, "circle", {"Q", "Backspace"});
  add(t, "square", {"LeftShift"});
  add(t, "triangle", {"F"});
  add(t, "l1", {"C"});
  add(t, "r1", {"R", "mouse_right"});
  add(t, "l2", {"Z"});
  add(t, "r2", {"X"});
  add(t, "l3", {"LeftCtrl"});
  add(t, "r3", {"mouse_middle"});
  add(t, "start", {"Escape", "Return"});
  add(t, "select", {"Tab"});
  add(t, "dpad_up", {"Up"});
  add(t, "dpad_down", {"Down"});
  add(t, "dpad_left", {"Left"});
  add(t, "dpad_right", {"Right"});
  add(t, "left_stick_up", {"W"});
  add(t, "left_stick_down", {"S"});
  add(t, "left_stick_left", {"A"});
  add(t, "left_stick_right", {"D"});
  add(t, "right_stick_up", {"I"});
  add(t, "right_stick_down", {"K"});
  add(t, "right_stick_left", {"J"});
  add(t, "right_stick_right", {"L"});
  add(t, "toggle_fullscreen", {"F11"});
  add(t, "toggle_mouse_capture", {"F10"});
  return t;
}

BindingTable default_gamepad_bindings() {
  BindingTable t;
  add(t, "cross", {"south"});
  add(t, "circle", {"east"});
  add(t, "square", {"west"});
  add(t, "triangle", {"north"});
  add(t, "l1", {"left_shoulder"});
  add(t, "r1", {"right_shoulder"});
  add(t, "l2", {"left_trigger"});
  add(t, "r2", {"right_trigger"});
  add(t, "l3", {"left_stick"});
  add(t, "r3", {"right_stick"});
  add(t, "start", {"start"});
  add(t, "select", {"back"});
  add(t, "dpad_up", {"dpad_up"});
  add(t, "dpad_down", {"dpad_down"});
  add(t, "dpad_left", {"dpad_left"});
  add(t, "dpad_right", {"dpad_right"});
  add(t, "left_stick_up", {"left_y-"});
  add(t, "left_stick_down", {"left_y+"});
  add(t, "left_stick_left", {"left_x-"});
  add(t, "left_stick_right", {"left_x+"});
  add(t, "right_stick_up", {"right_y-"});
  add(t, "right_stick_down", {"right_y+"});
  add(t, "right_stick_left", {"right_x-"});
  add(t, "right_stick_right", {"right_x+"});
  return t;
}

}  // namespace lantern::input
