#include <doctest/doctest.h>

#include <cmath>

#include "input/deadzone.h"

using namespace lantern::input;

TEST_CASE("radial dead zone zeroes small deflections") {
  StickDeadzone dz{0.2f, 0.9f};
  auto v = apply_radial_deadzone({0.1f, 0.1f}, dz);
  CHECK(v.x == 0.0f);
  CHECK(v.y == 0.0f);
}

TEST_CASE("radial dead zone saturates past outer and preserves direction") {
  StickDeadzone dz{0.2f, 0.9f};
  auto v = apply_radial_deadzone({0.95f, 0.0f}, dz);
  CHECK(v.x == doctest::Approx(1.0f));
  CHECK(v.y == doctest::Approx(0.0f));
  auto d = apply_radial_deadzone({0.7f, 0.7f}, dz);  // |v| ~ 0.99 -> saturated diagonal
  CHECK(std::hypot(d.x, d.y) == doctest::Approx(1.0f));
  CHECK(d.x == doctest::Approx(d.y));
}

TEST_CASE("radial dead zone rescales linearly between inner and outer") {
  StickDeadzone dz{0.2f, 1.0f};
  auto v = apply_radial_deadzone({0.0f, -0.6f}, dz);
  CHECK(v.y == doctest::Approx(-0.5f));
}

TEST_CASE("trigger dead zone") {
  TriggerDeadzone dz{0.1f, 0.9f};
  CHECK(apply_trigger_deadzone(0.05f, dz) == 0.0f);
  CHECK(apply_trigger_deadzone(0.5f, dz) == doctest::Approx(0.5f));
  CHECK(apply_trigger_deadzone(1.0f, dz) == doctest::Approx(1.0f));
  CHECK(apply_trigger_deadzone(-1.0f, dz) == 0.0f);
}

TEST_CASE("degenerate dead zone does not divide by zero") {
  StickDeadzone dz{0.5f, 0.5f};
  auto v = apply_radial_deadzone({1.0f, 0.0f}, dz);
  CHECK(std::isfinite(v.x));
}
