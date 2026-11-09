#version 410 core

// Jak Enhanced: the Hi-Z pyramid (post.hiz). Level 0: the view depth of each pixel from the depth
// copy, with the sky, the clouds and the sea itself far away (they reflect nothing); every other
// level (half the size, rounded down): the nearest of the 2x2 texels under it in the level before,
// 3 wide at an odd edge so the last texel covers the leftover row/column. The water's reflection
// marches it a tile at a time instead of a pixel at a time.

uniform sampler2D tex_T0;  // level 0: the depth copy; else the pyramid (base level = the one before)
uniform sampler2D tex_T1;  // level 0: the depth right after the mid/far ocean (black when none)
uniform int has_sea;       // tex_T1 is there
uniform int mode;          // 0: level 0 from the depth, 1: a level from the one before
uniform ivec2 src_size;    // mode 1: the level before's size
uniform vec4 proj;         // a, b, fx, fy

out float z_out;

const float kFar = 1e30;

float view_z(float d) {
  return abs(proj.y / (d * 2.0 - 1.0 - proj.x));
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  if (mode == 0) {
    float d = texelFetch(tex_T0, p, 0).r;
    float fd = has_sea == 1 ? texelFetch(tex_T1, p, 0).r : 0.0;
    z_out = (d <= 1e-7 || (fd > 1e-7 && abs(fd - d) < 1e-7)) ? kFar : view_z(d);
    return;
  }
  ivec2 s = p * 2;
  ivec2 hi = src_size - 1;
  bool wide_x = s.x + 2 == hi.x;
  bool wide_y = s.y + 2 == hi.y;
  float z = kFar;
  for (int dy = 0; dy < 3; dy++) {
    for (int dx = 0; dx < 3; dx++) {
      if ((dx < 2 || wide_x) && (dy < 2 || wide_y)) {
        z = min(z, texelFetch(tex_T0, min(s + ivec2(dx, dy), hi), 0).r);
      }
    }
  }
  z_out = z;
}
