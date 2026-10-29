#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in vec3 rgba_base;
layout (location = 3) in int time_of_day_index;
layout (location = 4) in vec4 normal_in;  // Enhanced
layout (location = 5) in int enh_flag_in;  // Enhanced: 1 = a detailed model stands in near the camera

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
uniform float enh_foliage;  // Enhanced: 1 on grass and leaves (cutout draws)
uniform vec4 enh_player;    // Jak (world, game units), w: the radius the grass bends in (0 = off)
uniform vec4 enh_pushers[8];  // enemies, passers-by and vehicles near Jak, same layout
uniform int enh_pusher_count;
uniform sampler2D tex_T4;  // Enhanced: the crush map (EnhancedScenery)
uniform vec4 enh_crush;    // its corner (world x, z), its size (game units), on

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
out vec3 enh_normal;
out vec3 enh_pos;
flat out int enh_lod_kind;
flat out vec2 enh_xfade;  // (the detailed models' swaps: none here)

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
  if (enh_foliage > 0.0 && enh_player.w > 0.0) {
    // pushed aside and pressed down where Jak (and whoever is near him) walks: the tips most, the
    // feet not at all; the strongest push wins
    float tip = clamp(1.0 - tex_coord_in.y / 4096.0, 0.0, 1.0);
    vec2 offset = vec2(0.0);
    float press = 0.0;
    for (int i = -1; i < enh_pusher_count; i++) {
      vec4 pusher = i < 0 ? enh_player : enh_pushers[i];
      vec3 d = position_in - pusher.xyz;
      float hd = length(d.xz);
      float near = 1.0 - smoothstep(pusher.w * 0.25, pusher.w, hd);
      near *= 1.0 - smoothstep(1.5 * 4096.0, 3.0 * 4096.0, abs(d.y));
      vec2 away = hd > 1.0 ? d.xz / hd : vec2(1.0, 0.0);
      offset += away * near;
      press = max(press, near);
    }
    float len = length(offset);
    offset = len > 1.0 ? offset / len : offset;
    // where someone went through a little while ago the grass still lies, rising back
    if (enh_crush.w > 0.0) {
      vec2 cuv = (position_in.xz - enh_crush.xy) / enh_crush.z;
      if (all(greaterThan(cuv, vec2(0.0))) && all(lessThan(cuv, vec2(1.0)))) {
        vec3 c = texture(tex_T4, cuv).xyz;
        float lie = length(c.xy) * (1.0 - smoothstep(1.5, 3.0, abs(c.z - position_in.y / 4096.0)));
        if (lie > press) {
          offset = c.xy / max(length(c.xy), 1e-4) * lie;
          press = lie;
        }
      }
    }
    float push = tip * tip;
    world.xz += offset * push * 0.45 * 4096.0;
    world.y -= press * push * 0.25 * 4096.0;
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
  enh_normal = normal_in.xyz;
  enh_pos = vert;
  enh_lod_kind = enh_flag_in;
  enh_xfade = vec2(0.0);
  tex_coord.xy /= 4096;
}
