#version 410 core

in float side;
in vec4 ribbon_color;

out vec4 color;

void main() {
  // a glow across it with a white-hot thread down the middle (added to the picture; alpha untouched)
  float s2 = side * side;
  float glow = exp(-s2 * 3.0) * (1.0 - s2);
  float thread = exp(-s2 * 30.0);
  color = vec4(mix(ribbon_color.rgb, vec3(1.0), thread * 0.7) * glow * ribbon_color.a, 0.0);
}
