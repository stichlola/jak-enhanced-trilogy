#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in int time_of_day_index;

uniform vec4 hvdf_offset;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform int decal;
// Enhanced: the leaves flutter (Tie3::render_tree_wind): xyz the tree's foot (world, m), w how far
// (game units, 0 = still); in the tree's own space, more the higher up its first 2 m
uniform vec4 enh_tie_flutter;
uniform float enh_tie_time;

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;

void main() {
  vec3 p = position_in;
  if (enh_tie_flutter.w > 0.0) {
    float t = enh_tie_time;
    vec2 wp = enh_tie_flutter.xz + position_in.xz / 4096.0;
    float f = clamp(position_in.y / 8192.0, 0.0, 1.0) * enh_tie_flutter.w;
    p += vec3(sin(t * 6.1 + wp.x * 1.7 + wp.y * 1.3), sin(t * 7.3 + wp.x * 1.1 - wp.y * 1.9) * 0.6,
              sin(t * 5.3 - wp.x * 1.4 + wp.y * 1.6)) * f;
  }
  vec4 transformed = -camera[3];
  transformed -= camera[0] * p.x;
  transformed -= camera[1] * p.y;
  transformed -= camera[2] * p.z;
  float Q = fog_constant / transformed.w;

  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);

  // perspective divide!
  transformed.xyz *= Q;
  // offset
  transformed.xyz += hvdf_offset.xyz;
  // correct xy offset
  transformed.xy -= (2048.);
  // correct z scale
  transformed.z /= (8388608);
  transformed.z -= 1;
  // correct xy scale
  transformed.x /= (256);
  transformed.y /= -(128);
  // hack
  transformed.xyz *= transformed.w;
  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;

  // time of day lookup
  fragment_color = texelFetch(tex_T10, time_of_day_index, 0);
  // color adjustment
  fragment_color *= 2;
  fragment_color.a *= 2;

  if (decal == 1) {
    // tfrag/tie always use TCC=RGB, so even with decal, alpha comes from fragment.
    fragment_color.xyz = vec3(1.0, 1.0, 1.0);
  }

  tex_coord = tex_coord_in;
}