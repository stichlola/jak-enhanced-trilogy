#version 410 core

out vec4 color;
in vec3 vtx_color;
in vec2 vtx_st;
in float fog;


uniform sampler2D tex_T0;

uniform vec4 fog_color;
uniform int ignore_alpha;

uniform int decal_enable;

uniform int gfx_hack_no_tex;
uniform float enh_fade_in = 1.0;  // Enhanced: the level just drawn dithers in (Merc2, fg.fade_in)
float enh_dither() {
  ivec2 p = ivec2(gl_FragCoord.xy) & 3;
  // (the 4x4 Bayer matrix by bit math, tfrag3.frag's: no local array)
  int q = p.x ^ p.y;
  int m = ((q & 1) << 3) | ((p.y & 1) << 2) | (q & 2) | ((p.y & 2) >> 1);
  return (float(m) + 0.5) / 16.0;
}


void main() {
  if (enh_fade_in < 1.0 && enh_fade_in <= enh_dither()) {
    discard;
  }
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, vtx_st);

    color.a = T0.a;
    color.rgb = T0.rgb * vtx_color;
    color *= 2;
  } else {
    color.rgb = vtx_color;
    color.a = 1;
  }
}
