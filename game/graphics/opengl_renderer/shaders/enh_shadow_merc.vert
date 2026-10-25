#version 410 core

// Jak 3 Enhanced: the characters in the sun shadow map. Skinned like merc2.vert (the bones put
// them in camera space), then back to the world with the camera and drawn from the sun.

layout (location = 0) in vec3 position_in;
layout (location = 2) in vec3 weights_in;
layout (location = 3) in vec2 st_in;
layout (location = 5) in uvec3 mats;

uniform mat4 light_mvp;
uniform mat3 view_to_world;
uniform vec3 cam_trans;

struct MercMatrixData {
  mat4 X;
  mat3 R;
  vec4 pad;
};

layout (std140) uniform ub_bones {
  MercMatrixData bones[128];
};

out vec2 st;

void main() {
  vec4 p = vec4(position_in, 1);
  vec4 vtx_pos = -bones[mats[0]].X * p * weights_in[0];
  if (weights_in[1] > 0) {
    vtx_pos += -bones[mats[1]].X * p * weights_in[1];
  }
  if (weights_in[2] > 0) {
    vtx_pos += -bones[mats[2]].X * p * weights_in[2];
  }
  // merc's camera space is the view space negated (see vtx_view in merc2.vert)
  vec3 world = cam_trans - view_to_world * vtx_pos.xyz;
  st = st_in;
  gl_Position = light_mvp * vec4(world, 1.0);
}
