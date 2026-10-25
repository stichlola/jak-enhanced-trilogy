#version 410 core

// Jak 3 Enhanced: sun shadow map. The levels' static geometry (world positions, as uploaded by
// the loader) drawn from the sun with an orthographic projection; depth only.

layout (location = 0) in vec3 position_in;

uniform mat4 light_mvp;

out vec3 world;

void main() {
  world = position_in / 4096.0;
  gl_Position = light_mvp * vec4(position_in, 1.0);
}
