#version 410 core

// Enhanced: a detailed model's far picture (shrub_imp.vert): the three nearest pictures of its
// atlas blended, lit by its normals atlas. Otherwise as shrub.frag (swaps, the game's protos
// taking over at the far end, rain, fog).

layout (location = 0) out vec4 color;
layout (location = 1) out vec4 enh_normal_out;  // world normal, a = 0 for none

in vec4 fragment_color;
in float fogginess;
in vec3 enh_pos;
in vec2 enh_imp_local[3];
flat in vec2 enh_imp_cell[3];
flat in vec3 enh_imp_weight;
flat in mat3 enh_imp_turn;
flat in vec3 enh_imp_view;
flat in vec2 enh_xfade;

uniform sampler2D tex_T0;  // the pictures
uniform sampler2D tex_T1;  // their normals (the model's space, 0..1)
uniform float enh_imp_grid;
uniform int enh_imp_normals;  // 1 when there's a normals atlas
uniform vec2 enh_lod;
uniform float enh_time;
uniform float alpha_min;
uniform float alpha_max;
uniform vec4 fog_color;
uniform float enh_foliage;
uniform float enh_wet;

float noise() {
  return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

void main() {
  // the game's proto takes over at the far end (its own dither: shrub.frag's)
  if (enh_lod.y > 0.0) {
    ivec2 p = ivec2(gl_FragCoord.xy) & 3;
    int m[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    float dither = fract((float(m[p.y * 4 + p.x]) + 0.5) / 16.0);
    if (smoothstep(enh_lod.x, enh_lod.y, length(enh_pos)) > dither) {
      discard;
    }
  }
  if (enh_xfade.y != 0.0 && (noise() < enh_xfade.x) != (enh_xfade.y > 0.0)) {
    discard;
  }

  vec4 T0 = vec4(0.0);
  vec3 n = vec3(0.0);
  float wsum = 0.0;
  for (int k = 0; k < 3; k++) {
    vec2 l = enh_imp_local[k];
    if (enh_imp_weight[k] <= 0.0 || abs(l.x) > 1.0 || abs(l.y) > 1.0) {
      continue;
    }
    // the atlas cell (the png loads top row first: v runs down from the bake's bottom-up rows)
    vec2 t = (enh_imp_cell[k] + l * 0.5 + 0.5) / enh_imp_grid;
    t.y = 1.0 - t.y;
    vec4 s = texture(tex_T0, t);
    T0 += s * enh_imp_weight[k];
    if (enh_imp_normals != 0) {
      n += (texture(tex_T1, t).xyz * 2.0 - 1.0) * s.a * enh_imp_weight[k];
    }
    wsum += enh_imp_weight[k];
  }
  if (wsum <= 0.0) {
    discard;
  }
  T0 /= wsum;
  // lit as the model: its normals, the side toward the camera (blades show both); without the
  // atlas, mostly up
  n = enh_imp_normals != 0 && dot(n, n) > 1e-4 ? normalize(n) : normalize(vec3(0.0, 0.8, 0.0) + enh_imp_view * 0.4);
  if (dot(n, enh_imp_view) < 0.0) {
    n = -n;
  }
  enh_normal_out = vec4(normalize(enh_imp_turn * n) * 0.5 + 0.5, 1.0);

  color = fragment_color * T0;
  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }
  if (enh_wet > 0.0) {
    float w = enh_wet * (enh_foliage > 0.5 ? 1.0 : 0.7);
    color.xyz = mix(color.xyz, color.xyz * color.xyz * 1.8, 0.5 * w) * (1.0 - 0.22 * w);
  }
  color.xyz = mix(color.xyz, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
