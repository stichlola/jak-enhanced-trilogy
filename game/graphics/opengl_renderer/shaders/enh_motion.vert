#version 410 core

// Jak Enhanced Trilogy: the TAA's motion vectors for the characters (EnhancedPost::do_motion). Each
// vertex skinned like merc2.vert twice: with this frame's bones (camera space now) and with last
// frame's bones of the same model (camera space then). Drawn with the post's projection, no depth:
// the fragment shader keeps only what the scene's depth shows in front.

layout (location = 0) in vec3 position_in;
layout (location = 2) in vec3 weights_in;
layout (location = 5) in uvec3 mats;

struct MercMatrixData {
  mat4 X;
  mat3 R;
  vec4 pad;
};

layout (std140) uniform ub_bones {
  MercMatrixData bones[128];
};

layout (std140) uniform ub_prev_bones {
  MercMatrixData prev_bones[128];
};

uniform vec4 proj;  // a, b, fx, fy (see enh_ssao.frag)

out vec3 cur_v;   // view space now
out vec3 prev_v;  // view space last frame

void main() {
  vec4 p = vec4(position_in, 1);
  vec4 c = bones[mats[0]].X * p * weights_in[0];
  vec4 q = prev_bones[mats[0]].X * p * weights_in[0];
  if (weights_in[1] > 0) {
    c += bones[mats[1]].X * p * weights_in[1];
    q += prev_bones[mats[1]].X * p * weights_in[1];
  }
  if (weights_in[2] > 0) {
    c += bones[mats[2]].X * p * weights_in[2];
    q += prev_bones[mats[2]].X * p * weights_in[2];
  }
  cur_v = c.xyz;
  prev_v = q.xyz;
  // the post's projection (uv = 0.5 f xy / z + 0.5); z = 0: never clipped by depth
  gl_Position = vec4(proj.z * c.x, proj.w * c.y, 0.0, c.z);
}
