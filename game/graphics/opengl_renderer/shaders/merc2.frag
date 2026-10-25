#version 410 core

layout (location = 0) out vec4 color;
layout (location = 1) out vec4 enh_normal_out;  // Enhanced: world normal, a = 0 for none
in vec4 vtx_color;
in vec2 vtx_st;
in float fog;

uniform sampler2D tex_T0;

uniform vec4 fog_color;
uniform int ignore_alpha;
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;

uniform int decal_enable;

uniform int gfx_hack_no_tex;

in vec3 vtx_nrm;
in vec3 vtx_key;
in vec3 vtx_view;
in vec3 vtx_key_color;
in vec3 enh_pos;
uniform mat3 enh_world_to_view;
uniform vec2 enh_rim_spec;  // Enhanced: rim light, highlight strength (0 = vanilla)

void main() {
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, vtx_st);
    // all merc is tcc=rgba and modulate
    if (decal_enable == 0) {
      color = vtx_color * T0 * 2;
    } else {
      color = T0;
    }
    color.a *= 2;
  } else {
    color.rgb = vtx_color.rgb;

    if (decal_enable == 0) {
      color.a = vtx_color.a * 2;
    } else {
      color.a = 1;
    }
  }

  if (light_dir1_fade_en.w > 0) {
    color.a = light_dir0_fade.w;
  } else if (light_dir1_fade_en.w < 0) {
    color.a *= light_dir0_fade.w;
  }


  if (ignore_alpha == 0 && color.w < 0.128) {
    discard;
  }
  // Enhanced: the normal for the post pass (world), as the levels write it: the smooth one (view
  // space here) where it stays near the triangle's own face, else the face's
  {
    vec3 f = cross(dFdx(enh_pos), dFdy(enh_pos));
    float flen = length(f);
    vec3 sn = transpose(enh_world_to_view) * vtx_nrm;
    float len = length(sn);
    vec3 n = vec3(0.0);
    if (flen > 0.0) {
      f /= flen;
      float d = len > 0.1 ? dot(sn / len, f) : 0.0;
      n = abs(d) > 0.82 ? sn / len * sign(d) : f;
    }
    enh_normal_out = flen > 0.0 ? vec4(n * 0.5 + 0.5, 1.0) : vec4(0.0);
  }

  // Enhanced: a rim of the key light around the silhouette, brighter on the lit side, and a soft
  // highlight (Blinn) from it
  if (decal_enable == 0 && (enh_rim_spec.x > 0.0 || enh_rim_spec.y > 0.0) && length(vtx_key) > 0.01) {
    vec3 n = normalize(vtx_nrm);
    vec3 v = normalize(vtx_view);
    vec3 l = normalize(vtx_key);
    float ndl = max(dot(n, l), 0.0);
    float rim = pow(1.0 - clamp(abs(dot(n, v)), 0.0, 1.0), 3.0) * (0.35 + 0.65 * ndl);
    float spec = pow(max(dot(n, normalize(l + v)), 0.0), 40.0) * ndl;
    vec3 key = clamp(vtx_key_color, 0.0, 1.5);
    color.rgb += key * (rim * enh_rim_spec.x * 0.5 + spec * enh_rim_spec.y * (0.3 + 0.7 * color.rgb));
  }

   color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
}
