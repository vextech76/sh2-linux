#include <doctest/doctest.h>

#include "app/cli.h"

using namespace lantern;
using namespace lantern::app;

TEST_CASE("defaults") {
  auto r = parse_cli({});
  CHECK(r.error.empty());
  CHECK(r.options.frames == 0);
  CHECK_FALSE(r.options.backend);
}

TEST_CASE("all options parse") {
  auto r = parse_cli({"--backend", "null", "--frames", "60", "--validation", "off", "--config", "/x.toml", "--iso",
                      "/g.iso", "--log-level", "debug", "--list-gpus", "--print-config", "--exercise-swapchain"});
  REQUIRE(r.error.empty());
  CHECK(r.options.backend == RenderBackend::Null);
  CHECK(r.options.frames == 60);
  CHECK(r.options.validation == Validation::Off);
  CHECK(r.options.config_path == "/x.toml");
  CHECK(r.options.iso_path == "/g.iso");
  CHECK(r.options.log_level == "debug");
  CHECK(r.options.list_gpus);
  CHECK(r.options.print_config);
  CHECK(r.options.exercise_swapchain);
}

TEST_CASE("errors") {
  CHECK_FALSE(parse_cli({"--frames"}).error.empty());
  CHECK_FALSE(parse_cli({"--frames", "12x"}).error.empty());
  CHECK_FALSE(parse_cli({"--backend", "opengl"}).error.empty());
  CHECK_FALSE(parse_cli({"--what"}).error.empty());
  CHECK_FALSE(parse_cli({"--log-level", "loud"}).error.empty());
}
