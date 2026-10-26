#version 410 core

// Jak Enhanced Trilogy: the characters in the sun shadow map, depth only; the see-through parts of
// alpha-tested textures don't cast (alpha_ref < 0: solid).

in vec2 st;
uniform sampler2D tex_T0;
uniform float alpha_ref;

void main() {
  if (alpha_ref >= 0.0 && texture(tex_T0, st).a < alpha_ref) {
    discard;
  }
}
