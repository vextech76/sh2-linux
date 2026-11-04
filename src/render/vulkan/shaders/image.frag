#version 450

// Presents the emulated display (an RGBA texture) inside the letterboxed viewport.
layout(push_constant) uniform Push {
  vec2 viewport_origin;
  vec2 viewport_size;
  float time;
} pc;
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(location = 0) out vec4 out_color;

void main() {
  vec2 uv = (gl_FragCoord.xy - pc.viewport_origin) / pc.viewport_size;
  out_color = vec4(texture(frame, uv).rgb, 1.0);
}
