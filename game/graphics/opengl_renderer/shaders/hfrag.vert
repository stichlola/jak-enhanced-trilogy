#version 410 core

layout (location = 0) in float position_in;
layout (location = 1) in int time_of_day_index;
layout (location = 2) in ivec2 uv;
layout (location = 3) in int vi;

uniform vec4 hvdf_offset;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
// uniform int decal;
uniform float fog_hack_threshold;

out vec4 fragment_color;
out vec2 tex_coord;
out float fogginess;
out vec2 world_xz;  // Enhanced: for the world-space sand
uniform sampler2D tex_T2;  // Enhanced: the grid's heights (Hfrag.cpp), 1e30 where it has none
out vec3 enh_normal;       // Enhanced: smooth world normal, from the neighbors' heights
out vec3 enh_pos;          // Enhanced: world position relative to the camera

float enh_height(int x, int z, float own) {
  float h = texelFetch(tex_T2, ivec2(clamp(x, 0, 511), clamp(z, 0, 511)), 0).r;
  return h > 1e29 ? own : h;
}

void main() {
  int vx = vi % 512;
  int vz = vi / 512;

  float dx = enh_height(vx + 1, vz, position_in) - enh_height(vx - 1, vz, position_in);
  float dz = enh_height(vx, vz + 1, position_in) - enh_height(vx, vz - 1, position_in);
  enh_normal = normalize(vec3(-dx, 2.0 * 32768.0, -dz));
  enh_pos = vec3(32768.0 * vx - cam_trans.x, position_in - cam_trans.y, 32768.0 * vz - cam_trans.z);

  tex_coord.x = (uv.x == 1) ? 1.f : 0.f;
  tex_coord.y = (uv.y == 1) ? 1.f : 0.f;
  world_xz = vec2(32768.f * vx, 32768.f * vz);

  vec3 vert = position_in - cam_trans.xyz;
  vec4 transformed = -pc_camera[3];
  transformed -= pc_camera[0] * (32768.f * vx - cam_trans.x);
  transformed -= pc_camera[1] * (position_in - cam_trans.y);
  transformed -= pc_camera[2] * (32768.f * vz - cam_trans.z);

  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);

  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;

  // time of day lookup
  fragment_color = texelFetch(tex_T10, time_of_day_index, 0);
  fragment_color.a = 1.0;
}
