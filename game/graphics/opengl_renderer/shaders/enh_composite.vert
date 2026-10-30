#version 410 core

layout (location = 0) in vec2 position_in;

uniform sampler2D tex_T0;  // scene
uniform sampler2D tex_T3;  // depth
uniform vec4 proj;
uniform vec2 texel;
uniform vec4 sky_sun;
uniform vec4 clouds;
uniform float lens_flare;
uniform int cloud_pass;

out vec2 tex_coord;
// (the same for every pixel of the frame: read once here, not per pixel in the fragment shader)
flat out float flare_vis;      // how much of the sun shows, for the lens flare
flat out vec3 white_balance;   // the scene's average color brought to grey

float luma(vec3 c) {
  return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
  gl_Position = vec4(position_in, 0, 1.0);
  tex_coord = (position_in + vec2(1.0, 1.0)) * 0.5;

  flare_vis = 0.0;
  white_balance = vec3(1.0);
  if (cloud_pass == 1) {
    return;
  }
  // the sky around the sun's disk, not hidden behind a wall or the clouds
  if (lens_flare > 0.0 && sky_sun.z > 0.0 && sky_sun.w > 0.0 && clouds.w < 0.5) {
    vec2 suv = 0.5 * vec2(proj.z * sky_sun.x, proj.w * sky_sun.y) / sky_sun.z + 0.5;
    if (all(greaterThan(suv, vec2(-0.3))) && all(lessThan(suv, vec2(1.3)))) {
      for (int i = 0; i < 12; i++) {
        float r = sqrt((float(i) + 0.5) / 12.0);
        float a = float(i) * 2.39996;
        vec2 q = suv + r * vec2(cos(a), sin(a)) * vec2(texel.x / texel.y, 1.0) * 0.012;
        if (all(greaterThan(q, vec2(0.0))) && all(lessThan(q, vec2(1.0))) &&
            textureLod(tex_T3, q, 0.0).r <= 1e-7) {
          flare_vis += smoothstep(0.55, 0.95, luma(textureLod(tex_T0, q, 0.0).rgb));
        }
      }
      flare_vis /= 12.0;
    }
  }
  if (clouds.w < 0.99) {
    vec3 avg = vec3(0.0);
    for (int j = 0; j < 4; j++) {
      for (int i = 0; i < 5; i++) {
        avg += textureLod(tex_T0, vec2((float(i) + 0.5) / 5.0, (float(j) + 0.5) / 4.0), 0.0).rgb;
      }
    }
    avg = max(avg / 20.0, vec3(1e-3));
    white_balance = clamp(vec3(luma(avg)) / avg, vec3(0.8), vec3(1.25));
  }
}
