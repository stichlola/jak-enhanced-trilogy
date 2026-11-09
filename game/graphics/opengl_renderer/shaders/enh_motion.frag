#version 410 core

// Jak Enhanced Trilogy: a character pixel's motion vector (see enh_motion.vert): the uv offset to
// where this point of the model was last frame. Only where the model is what the scene shows (its
// distance within depth_tol of the depth buffer's): elsewhere the target keeps (64, 0), "none".

uniform sampler2D tex_depth;
uniform vec4 proj;      // a, b, fx, fy
uniform vec2 prev_f;    // last frame's fx, fy
uniform float depth_tol;

in vec3 cur_v;
in vec3 prev_v;
out vec4 color;

void main() {
  float d = texelFetch(tex_depth, ivec2(gl_FragCoord.xy), 0).r;
  float z = proj.y / (d * 2.0 - 1.0 - proj.x);
  if (abs(z - cur_v.z) > depth_tol * cur_v.z + 200.0 || prev_v.z <= 0.0) {
    discard;
  }
  vec2 uv = 0.5 * proj.zw * cur_v.xy / cur_v.z + 0.5;
  vec2 puv = 0.5 * prev_f * prev_v.xy / prev_v.z + 0.5;
  color = vec4(puv - uv, 0.0, 1.0);
}
