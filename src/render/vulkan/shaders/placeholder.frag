#version 450

// Visibly-marked placeholder: magenta/black "missing content" checkerboard with the word
// PLACEHOLDER. Shown whenever no game runtime output exists. Not game content.

layout(push_constant) uniform Push {
  vec2 viewport_origin;
  vec2 viewport_size;
  float time;
} pc;

layout(location = 0) out vec4 out_color;

// 5x7 glyphs, one row per entry, MSB = leftmost column. Order: P L A C E H O D R
const int kGlyphs[63] = int[63](
  30, 17, 17, 30, 16, 16, 16,   // P
  16, 16, 16, 16, 16, 16, 31,   // L
  14, 17, 17, 31, 17, 17, 17,   // A
  14, 17, 16, 16, 16, 17, 14,   // C
  31, 16, 16, 30, 16, 16, 31,   // E
  17, 17, 17, 31, 17, 17, 17,   // H
  14, 17, 17, 17, 17, 17, 14,   // O
  30, 17, 17, 17, 17, 17, 30,   // D
  30, 17, 17, 30, 20, 18, 17    // R
);
const int kText[11] = int[11](0, 1, 2, 3, 4, 5, 6, 1, 7, 4, 8);  // PLACEHOLDER

bool text_pixel(vec2 p, vec2 size) {
  const float cols = 11.0 * 6.0 - 1.0;
  const float rows = 7.0;
  float cell = floor(min(size.x * 0.8 / cols, size.y * 0.12 / rows));
  cell = max(cell, 1.0);
  vec2 extent = vec2(cols, rows) * cell;
  vec2 origin = floor((size - extent) * 0.5);
  vec2 g = floor((p - origin) / cell);
  if (g.x < 0.0 || g.y < 0.0 || g.x >= cols || g.y >= rows) return false;
  int ch = int(g.x) / 6;
  int col = int(g.x) - ch * 6;
  if (col == 5) return false;
  int bits = kGlyphs[kText[ch] * 7 + int(g.y)];
  return ((bits >> (4 - col)) & 1) != 0;
}

bool in_band(vec2 p, vec2 size) {
  float h = size.y * 0.2;
  return abs(p.y - size.y * 0.5) < h * 0.5;
}

void main() {
  vec2 p = gl_FragCoord.xy - pc.viewport_origin;
  vec2 size = pc.viewport_size;
  float checker_size = max(floor(size.y / 12.0), 4.0);
  vec2 c = floor((p + vec2(pc.time * 20.0, 0.0)) / checker_size);
  bool odd = mod(c.x + c.y, 2.0) > 0.5;
  vec3 color = odd ? vec3(0.55, 0.0, 0.55) : vec3(0.05, 0.0, 0.05);
  if (in_band(p, size)) color = vec3(0.0);
  if (text_pixel(p, size)) color = vec3(1.0);
  out_color = vec4(color, 1.0);
}
