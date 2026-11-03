#include "input/deadzone.h"

#include <algorithm>
#include <cmath>

namespace lantern::input {

Vec2 apply_radial_deadzone(Vec2 v, const StickDeadzone& dz) {
  const float mag = std::hypot(v.x, v.y);
  if (mag <= dz.inner || mag == 0.0f) return {};
  const float span = std::max(dz.outer - dz.inner, 1e-4f);
  const float scaled = std::clamp((mag - dz.inner) / span, 0.0f, 1.0f);
  return {v.x / mag * scaled, v.y / mag * scaled};
}

float apply_trigger_deadzone(float v, const TriggerDeadzone& dz) {
  v = std::clamp(v, 0.0f, 1.0f);
  if (v <= dz.inner) return 0.0f;
  const float span = std::max(dz.outer - dz.inner, 1e-4f);
  return std::clamp((v - dz.inner) / span, 0.0f, 1.0f);
}

}  // namespace lantern::input
