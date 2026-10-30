#version 410 core

layout (location = 0) out vec4 color;
layout (location = 1) out vec4 enh_normal_out;  // Enhanced: world normal, a = 0 for none
in vec3 enh_normal;
in vec3 enh_pos;  // world position (relative to the camera where known)

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;


// the smooth normal from the vertices where it stays within ~35 degrees of the triangle's own
// face (curved surfaces), else the face's (hard edges a shared strip vertex would round off)
uniform int enh_material;  // Enhanced: 0 plain, 1 metal, 2 glossy, 3 sand, 4 rock, 5 rocky ground (see EnhancedMaterials.h)

vec4 enh_pack_normal(vec3 n) {
  vec3 f = cross(dFdx(enh_pos), dFdy(enh_pos));
  float flen = length(f);
  float len = length(n);
  if (flen > 0.0) {
    f /= flen;
    if (len > 0.1) {
      n /= len;
      float d = dot(n, f);
      // (blended, not switched: a hard switch drew the lights' highlights as sharp patches)
      n = normalize(mix(f, n * sign(d), smoothstep(0.6, 0.85, abs(d))));
    } else {
      n = f;
    }
  } else if (len > 0.1) {
    n /= len;
  } else {
    return vec4(0.0);
  }
  // the material in the 2-bit alpha: 1 plain, 2/3 metal, 1/3 glossy (0: no normal); sand (plain)
  // as a normal 0.9 long (the composite draws the wind's ripples on it)
  float a = enh_material == 1 ? 2.0 / 3.0 : (enh_material == 2 ? 1.0 / 3.0 : 1.0);
  float tag_len = enh_material == 3 ? 0.9 : (enh_material == 4 ? 0.75 : (enh_material == 5 ? 0.6 : 1.0));
  return vec4(n * tag_len * 0.5 + 0.5, a);
}

// Enhanced: the detailed model stands in near the camera, this proto beyond, dithered across the
// band: the same pixels as shrub.frag leaves the model (EnhancedScenery)
flat in int enh_lod_kind;
uniform vec2 enh_lod;  // distance from the camera (0, 0 = off)
float enh_dither() {
  ivec2 p = ivec2(gl_FragCoord.xy) & 3;
  int m[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
  return fract((float(m[p.y * 4 + p.x]) + 0.5) / 16.0);
}

void main() {
  if (enh_lod_kind != 0 && enh_lod.y > 0.0 &&
      smoothstep(enh_lod.x, enh_lod.y, length(enh_pos)) <= enh_dither()) {
    discard;
  }
  enh_normal_out = enh_pack_normal(enh_normal);
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
  } else {
    color = fragment_color/2;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }

  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
