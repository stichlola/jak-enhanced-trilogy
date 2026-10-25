#version 410 core

// Jak 3 Enhanced: sun shadow map, depth only. What the background shaders cut away for the
// passages into the Jak 2 plaza (Shader.cpp, enh_in_cut) doesn't cast shadows either, nor do
// the see-through parts of alpha-tested textures (alpha_ref < 0: solid).

in vec3 world;
in vec2 st;
uniform int enh_cut;
uniform sampler2D tex_T0;
uniform float alpha_ref;

bool enh_in_cut(vec3 p) {
  if (p.x > 42.0 && p.x < 92.0 && p.y > 7.0 && p.y < 60.0 && p.z > 950.0 && p.z < 968.0)
    return true;
  vec2 a = vec2(229.0, 1146.0), b = vec2(200.0, 1206.0);
  vec2 ab = b - a;
  float t = clamp(dot(p.xz - a, ab) / dot(ab, ab), 0.0, 1.0);
  return length(p.xz - (a + ab * t)) < 6.5 && p.y > 4.6 && p.y < 16.0;
}

void main() {
  if (enh_cut != 0 && enh_in_cut(world)) {
    discard;
  }
  if (alpha_ref >= 0.0 && texture(tex_T0, st).a < alpha_ref) {
    discard;
  }
}
