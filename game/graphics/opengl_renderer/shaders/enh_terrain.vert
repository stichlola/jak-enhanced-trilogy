#version 410 core

// Enhanced: a private level's own ground (EnhancedTerrain). Lit, fogged and placed like shrub_inst.vert

layout (location = 0) in vec3 position_in;  // world, game units
layout (location = 1) in vec3 normal_in;
layout (location = 2) in vec4 mask_in;      // grass, rock, dirt; the rest sand

uniform vec4 hvdf_offset;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform sampler1D tex_T10;
uniform int enh_tod;
uniform float enh_base;

out vec4 fragment_color;
out float fogginess;
out vec3 enh_normal;
out vec3 enh_pos;
out vec3 enh_world;  // m
out vec3 enh_mask;

void main() {
  vec3 vert = position_in - cam_trans.xyz;
  vec4 transformed = -pc_camera[3];
  transformed -= pc_camera[0] * vert.x;
  transformed -= pc_camera[1] * vert.y;
  transformed -= pc_camera[2] * vert.z;
  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;

  fragment_color = vec4(vec3(enh_base), 0.5) * texelFetch(tex_T10, enh_tod, 0) * 4;
  enh_normal = normal_in;
  enh_pos = vert;
  enh_world = position_in / 4096.0;
  enh_mask = mask_in.xyz;
}
