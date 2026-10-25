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
uniform int enh_material;  // Enhanced: 0 plain, 1 metal, 2 glossy (see EnhancedMaterials.h)

vec4 enh_pack_normal(vec3 n) {
  vec3 f = cross(dFdx(enh_pos), dFdy(enh_pos));
  float flen = length(f);
  float len = length(n);
  if (flen > 0.0) {
    f /= flen;
    if (len > 0.1) {
      n /= len;
      float d = dot(n, f);
      n = abs(d) > 0.82 ? n * sign(d) : f;
    } else {
      n = f;
    }
  } else if (len > 0.1) {
    n /= len;
  } else {
    return vec4(0.0);
  }
  // the material in the 2-bit alpha: 1 plain, 2/3 metal, 1/3 glossy (0: no normal)
  float a = enh_material == 1 ? 2.0 / 3.0 : (enh_material == 2 ? 1.0 / 3.0 : 1.0);
  return vec4(n * 0.5 + 0.5, a);
}

void main() {
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
