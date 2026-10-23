#version 410 core

// Jak 3 Enhanced: god rays. The bright sky (pixels with no depth) is smeared along the line to
// the sun's position on screen, so it streams through gaps between buildings and rocks.

uniform sampler2D tex_T0;  // frame
uniform sampler2D tex_T1;  // depth
uniform vec2 sun_uv;
uniform float threshold;
uniform float fade;  // 0..1 as the sun leaves the screen

in vec2 tex_coord;
out vec4 color;

const int STEPS = 40;

void main() {
  vec2 delta = (sun_uv - tex_coord) / float(STEPS);
  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  vec2 uv = tex_coord + delta * noise;
  vec3 sum = vec3(0.0);
  float decay = 1.0;
  for (int i = 0; i < STEPS; i++) {
    if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0 && texture(tex_T1, uv).r <= 1e-7) {
      vec3 s = texture(tex_T0, uv).rgb;
      float lum = max(s.r, max(s.g, s.b));
      sum += s * max(lum - threshold, 0.0) / max(1.0 - threshold, 1e-3) * decay;
    }
    decay *= 0.96;
    uv += delta;
  }
  // weaker far from the sun
  float falloff = 1.0 - clamp(length((tex_coord - sun_uv) * vec2(1.6, 1.0)) / 1.4, 0.0, 1.0);
  color = vec4(sum / float(STEPS) * 4.0 * fade * falloff, 1.0);
}
