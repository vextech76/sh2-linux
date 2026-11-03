#pragma once

namespace lantern::input {

struct StickDeadzone {
  float inner = 0.15f;  // magnitudes below this read as zero
  float outer = 0.95f;  // magnitudes above this read as full deflection
};

struct TriggerDeadzone {
  float inner = 0.05f;
  float outer = 1.0f;
};

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

// Radial dead zone: preserves direction, rescales magnitude from [inner, outer] to [0, 1].
Vec2 apply_radial_deadzone(Vec2 v, const StickDeadzone& dz);

// One-dimensional dead zone for triggers (input in [0, 1]).
float apply_trigger_deadzone(float v, const TriggerDeadzone& dz);

}  // namespace lantern::input
