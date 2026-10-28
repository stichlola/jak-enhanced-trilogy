#version 410 core

// Jak Enhanced Trilogy: god rays. The bright sky (pixels with no depth) is smeared along the line to
// the sun's position on screen, so it streams through gaps between buildings and rocks.
// Alpha: how much sun the haze gets (the desert's): far ridges standing between a pixel and the sun
// on screen keep it from the air behind them, so dark rays fan out of the mountains.

uniform sampler2D tex_T0;  // frame
uniform sampler2D tex_T1;  // depth
uniform vec2 sun_uv;
uniform float threshold;
uniform float fade;  // 0..1 as the sun leaves the screen
uniform float rays;  // 1: draw the god rays (the sun in front), 0: only the haze's shadow
uniform float haze;  // 1: the haze's shadow
uniform float sun_side;  // 1: sun_uv is the sun, -1: it's the point opposite (the sun behind)
uniform vec4 proj;   // a, b, fx, fy (see enh_ssao.frag)

in vec2 tex_coord;
out vec4 color;

const int STEPS = 64;  // (fewer shimmered as the sky's bright edges moved)

float haze_lit(float noise) {
  float d0 = texture(tex_T1, tex_coord).r;
  float dist = d0 > 1e-7 ? proj.y / (d0 * 2.0 - 1.0 - proj.x) / 4096.0 : 1e9;  // m
  vec2 dir = (sun_uv - tex_coord) * sun_side;
  dir *= 0.45 / max(length(dir), 1e-4);
  const int TAPS = 24;
  float blocked = 0.0, total = 0.0;
  for (int i = 0; i < TAPS; i++) {
    float w = 1.0 - 0.6 * float(i) / float(TAPS);  // (the nearer the ridge, the darker)
    total += w;
    vec2 q = tex_coord + dir * (float(i) + noise) / float(TAPS);
    if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0) {
      continue;  // (off screen: lit)
    }
    float dq = texture(tex_T1, q).r;
    float zq = dq > 1e-7 ? proj.y / (dq * 2.0 - 1.0 - proj.x) / 4096.0 : 1e9;
    // a ridge: far off (not Jak, not the rocks around him), in front of this pixel, and not the
    // game's sky clouds (they write depth too, kilometers away)
    blocked += w * smoothstep(40.0, 90.0, zq) * (zq < min(dist * 0.97, 3000.0) ? 1.0 : 0.0);
  }
  return 1.0 - smoothstep(0.04, 0.5, blocked / total);
}

void main() {
  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  float lit = haze > 0.0 ? haze_lit(noise) : 1.0;
  if (rays <= 0.0) {
    color = vec4(0.0, 0.0, 0.0, lit);
    return;
  }
  vec2 delta = (sun_uv - tex_coord) / float(STEPS);
  vec2 uv = tex_coord + delta * noise;
  vec3 sum = vec3(0.0);
  float decay = 1.0;
  for (int i = 0; i < STEPS; i++) {
    if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0 && texture(tex_T1, uv).r <= 1e-7) {
      vec3 s = texture(tex_T0, uv).rgb;
      float lum = max(s.r, max(s.g, s.b));
      // (eased in over the threshold: a pixel crossing it doesn't pop)
      sum += s * smoothstep(threshold, 1.0, lum) * decay;
    }
    decay *= 0.975;
    uv += delta;
  }
  // weaker far from the sun
  float falloff = 1.0 - clamp(length((tex_coord - sun_uv) * vec2(1.6, 1.0)) / 1.4, 0.0, 1.0);
  color = vec4(sum / float(STEPS) * 4.0 * fade * falloff, lit);
}
