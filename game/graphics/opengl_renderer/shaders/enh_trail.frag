#version 410 core

in float side;
in vec4 ribbon_color;

// 0 at night, 1 in full sun (look.shot_day): how much the ribbon covers what's behind it
uniform float day;

out vec4 color;

void main() {
  // a glow across it with a white-hot thread down the middle (added to the picture; alpha untouched)
  float s2 = side * side;
  float glow = exp(-s2 * 3.0) * (1.0 - s2);
  float thread = exp(-s2 * 30.0);
  // (by day: a smaller white thread, and the ribbon hides up to 85% of the pale ground behind it,
  // so it reads by its hue instead of clipping to white over sand; at night day = 0: added light)
  vec3 rgb = mix(ribbon_color.rgb, vec3(1.0), thread * mix(0.7, 0.3, day)) * glow * ribbon_color.a;
  float cover = clamp(glow * 1.4, 0.0, 1.0) * ribbon_color.a * day * 0.85;
  color = vec4(rgb, cover);
}
