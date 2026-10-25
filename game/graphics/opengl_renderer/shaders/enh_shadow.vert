#version 410 core

// Jak 3 Enhanced: sun shadow map. The levels' static geometry (world positions, as uploaded by
// the loader) drawn from the sun with an orthographic projection; depth only. The wind-blown
// trees also bring texture coordinates, for their leaves' cutouts.

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec2 st_in;

uniform mat4 light_mvp;

out vec3 world;
out vec2 st;

void main() {
  world = position_in / 4096.0;
  st = st_in;
  gl_Position = light_mvp * vec4(position_in, 1.0);
}
