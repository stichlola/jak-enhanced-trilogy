#version 410 core


layout (location = 0) out vec4 color;
layout (location = 1) out vec4 enh_normal_out;  // Enhanced: only the cloth writes it (the 0.52 tag)
uniform int enh_cloth;
in vec2 tex_coord;

uniform float alpha_reject;
uniform float color_mult;
uniform vec4 fog_color;
in float fog;
in vec4 fragment_color;

flat in uvec2 tex_info;

uniform int gfx_hack_no_tex;
uniform uint warp_sample_mode;

uniform sampler2D tex_T0;

vec4 sample_tex(vec2 coord, uint unit) {
  return texture(tex_T0, coord);
}

void main() {
  // 0x1 is tcc
  // 0x2 is decal
  // 0x4 is fog

  if (warp_sample_mode == 1 || gfx_hack_no_tex == 0) {
    vec4 T0 = sample_tex(tex_coord.xy, tex_info.x);
    if ((tex_info.y & 1u) == 0) {
      if ((tex_info.y & 2u) == 0) {
        // modulate + no tcc
        color.rgb = fragment_color.rgb * T0.rgb;
        color.a = fragment_color.a;
      } else {
        // decal + no tcc
        color.rgb = T0.rgb * 0.5;
        color.a = fragment_color.a;
      }
    } else {
      if ((tex_info.y & 2u) == 0) {
        // modulate + tcc
        color = fragment_color * T0;
      } else {
        // decal + tcc
        color.rgb = T0.rgb * 0.5;
        color.a = T0.a;
      }
    }
    color *= 2;
  } else {
    if ((tex_info.y & 1u) == 0) {
      if ((tex_info.y & 2u) == 0) {
        // modulate + no tcc
        color.rgb = fragment_color.rgb;
        color.a = fragment_color.a * 2;
      } else {
        // decal + no tcc
        color.rgb = vec3(1);
        color.a = fragment_color.a * 2;
      }
    } else {
      if ((tex_info.y & 2u) == 0) {
        // modulate + tcc
        color = fragment_color;
      } else {
        // decal + tcc
        color.rgb = vec3(0.5);
        color.a = 1.0;
      }
    }
  }
  color.rgb *= color_mult;

  if (color.a < alpha_reject) {
    discard;
  }
  // Enhanced: Jak 3's cloth: a 0.52 long normal marks it (its direction unused)
  enh_normal_out = enh_cloth != 0 ? vec4(0.5, 0.5 + 0.26, 0.5, 1.0) : vec4(0.0);
  if ((tex_info.y & 4u) != 0) {
    color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  }
}
