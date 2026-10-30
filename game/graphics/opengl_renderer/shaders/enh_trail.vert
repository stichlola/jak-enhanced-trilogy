#version 410 core

// Enhanced: the ribbons of a gun's shots (EnhancedParticles), placed in the world like the game's
// sprites, so they sit in the scene and the depth test works

layout (location = 0) in vec4 pos_side;  // world xyz (game units), -1..1 across the ribbon
layout (location = 1) in vec4 color_in;

uniform mat4 camera;
uniform vec4 hvdf_offset;
uniform float pfog0;

out float side;
out vec4 ribbon_color;

void main() {
  vec4 p = camera[3] + camera[0] * pos_side.x + camera[1] * pos_side.y + camera[2] * pos_side.z;
  float Q = pfog0 / p.w;
  p.xyz *= Q;
  p.xyz += hvdf_offset.xyz;
  p.xy -= 2048.0;
  p.z /= 8388608.0;
  p.z -= 1.0;
  p.x /= 256.0;
  p.y /= -128.0;
  p.xyz *= p.w;
  p.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = p;
  side = pos_side.w;
  ribbon_color = color_in;
}
