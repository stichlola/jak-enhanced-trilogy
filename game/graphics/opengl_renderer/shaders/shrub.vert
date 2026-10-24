#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in vec3 rgba_base;
layout (location = 3) in int time_of_day_index;

uniform vec4 hvdf_offset;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform int decal;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform float enh_wind;  // Enhanced: wind strength on this draw (0 = none)
uniform float enh_time;  // seconds

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;

void main() {
  // old system:
  // - load vf12
  // - itof0 vf12
  // - multiply with camera matrix (add trans)
  // - let Q = fogx / vf12.w
  // - xyz *= Q
  // - xyzw += hvdf_offset
  // - clip w.
  // - ftoi4 vf12
  // use in gs.
  // gs is 12.4 fixed point, set up with 2048.0 as the center.

  // the itof0 is done in the preprocessing step.  now we have floats.
  
  // Step 3, the camera transform
  vec3 world = position_in;
  if (enh_wind > 0.0) {
    // the top of a card (texture v near 0) sways, its foot (v near 1) stays put: two gusts
    // travelling across the world plus a quick flutter
    float tip = clamp(1.0 - tex_coord_in.y / 4096.0, 0.0, 1.0);
    vec2 wp = position_in.xz / 4096.0;
    float gust = sin(enh_time * 1.3 + wp.x * 0.11 + wp.y * 0.07) * 0.6 +
                 sin(enh_time * 2.1 + wp.x * 0.05 - wp.y * 0.13) * 0.4;
    float flutter = sin(enh_time * 7.0 + wp.x * 1.7 + wp.y * 1.3) * 0.25;
    float amount = enh_wind * tip * tip * 0.12 * 4096.0;  // up to ~12 cm at the tips
    world.xz += vec2(0.8, 0.6) * (gust + flutter) * amount;
  }
  vec3 vert = world - cam_trans.xyz;
  vec4 transformed = -pc_camera[3];
  transformed -= pc_camera[0] * vert.x;
  transformed -= pc_camera[1] * vert.y;
  transformed -= pc_camera[2] * vert.z;

  // do fog!
  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);

  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;

  // time of day lookup
  // start with the vertex color (only rgb, VIF filled in the 255.)
  fragment_color =  vec4(rgba_base, 1);
  // get the time of day multiplier
  vec4 tod_color = texelFetch(tex_T10, time_of_day_index, 0);
  // combine
  fragment_color *= tod_color * 4;

  if (decal == 1) {
    fragment_color.xyz = vec3(1.0, 1.0, 1.0);
  }

  tex_coord = tex_coord_in;
  tex_coord.xy /= 4096;
}
