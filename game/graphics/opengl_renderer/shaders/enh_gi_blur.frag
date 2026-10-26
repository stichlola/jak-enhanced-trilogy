#version 410 core

// Jak Enhanced Trilogy: depth-aware 1D blur of the indirect light (run horizontally, then vertically).

uniform sampler2D tex_T0;  // indirect light
uniform sampler2D tex_T1;  // depth
uniform vec4 proj;
uniform vec2 step;

in vec2 tex_coord;
out vec4 gi_out;

float view_z(float d) {
  return abs(proj.y / (d * 2.0 - 1.0 - proj.x));
}

void main() {
  float z0 = view_z(texture(tex_T1, tex_coord).r);
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  for (int i = -5; i <= 5; i++) {
    vec2 uv = tex_coord + step * float(i) * 1.5;
    float z = view_z(texture(tex_T1, uv).r);
    float w = exp(-float(i * i) / 24.0) * exp(-abs(z - z0) / (0.03 * z0 + 1.0));
    sum += texture(tex_T0, uv).rgb * w;
    wsum += w;
  }
  gi_out = vec4(sum / max(wsum, 1e-5), 1.0);
}
