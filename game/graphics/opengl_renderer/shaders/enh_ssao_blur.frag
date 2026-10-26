#version 410 core

// Jak Enhanced Trilogy: depth-aware 1D blur of the occlusion (run horizontally, then vertically).

uniform sampler2D tex_T0;  // occlusion
uniform sampler2D tex_T1;  // depth
uniform vec4 proj;         // a, b, fx, fy
uniform vec2 step;

in vec2 tex_coord;
out float ao_out;

float view_z(float d) {
  return abs(proj.y / (d * 2.0 - 1.0 - proj.x));
}

void main() {
  float z0 = view_z(texture(tex_T1, tex_coord).r);
  float sum = 0.0;
  float wsum = 0.0;
  for (int i = -4; i <= 4; i++) {
    vec2 uv = tex_coord + step * float(i) * 1.5;
    float z = view_z(texture(tex_T1, uv).r);
    float w = exp(-float(i * i) / 18.0) * exp(-abs(z - z0) / (0.03 * z0 + 1.0));
    sum += texture(tex_T0, uv).r * w;
    wsum += w;
  }
  ao_out = sum / max(wsum, 1e-5);
}
