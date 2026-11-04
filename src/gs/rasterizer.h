#pragma once

#include <compare>
#include <cstdint>
#include <map>

#include "gs/gs.h"

namespace lantern::gs {

// Reference software rasterizer for GS primitives. Correctness first: nearest-texel sampling,
// per-pixel tests and blending as the GS defines them. Accurate-but-slow by design; GPU
// translation can be validated against it later.
class Rasterizer {
 public:
  void draw(Gs& gs, const Vertex* v, int count, uint64_t prim);

  struct Stats {
    uint64_t triangles = 0, sprites = 0, lines = 0, points = 0, pixels_written = 0, pixels_tested = 0;
    std::map<uint32_t, uint64_t> texture_samples;  // PSM -> textured pixels (which formats the game uses)
    // Texture descriptors actually sampled: (tbp, tbw, psm, tw, th) -> textured pixels.
    struct TexDesc {
      uint32_t tbp = 0, tbw = 0, psm = 0, tw = 0, th = 0;
      auto operator<=>(const TexDesc&) const = default;
    };
    std::map<TexDesc, uint64_t> textures;
  };
  const Stats& stats() const { return stats_; }

 private:
  struct Attr;
  void shade(Gs& gs, int x, int y, const Attr& a);
  Stats stats_;
};

}  // namespace lantern::gs
