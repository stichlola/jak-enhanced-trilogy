#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in ivec2 tod_sway;  // the time of day index; Enhanced: .y the TIE foliage's sway
layout (location = 3) in vec4 normal_in;  // Enhanced

uniform vec4 hvdf_offset;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform int decal;
// Enhanced: the static TIE trees in the wind (EnhancedTieWind.h): x = seconds, y = the wind now
// (EnhancedWind::now, 0 = still). The weight is in the vertices, so tfrag (all 0) never moves
uniform vec4 enh_tie_wind;
// 0 = still (as the game), 1 = sway; >= 10 paints the weights (blue leaves, green trunks, by height)
uniform float tune_tie_wind = 1.0;

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
out vec3 enh_normal;
out vec3 enh_pos;
flat out int enh_lod_kind;  // Enhanced: 1 = a detailed model stands in near the camera (TIE only)

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


  // Enhanced: the trees lean from the foot, more the higher up (the shrubs' gust field), the leaves
  // flutter on top
  vec3 world = position_in;
  int sway = tod_sway.y;
  float sway_h = float(sway & 0x7fff) * (12.0 / 32767.0);  // meters above the plant's foot
  if (sway != 0 && tune_tie_wind > 0.0 && tune_tie_wind < 10.0) {
    float t = enh_tie_wind.x;
    vec2 wp = position_in.xz / 4096.0;
    float gust = sin(t * 1.3 + wp.x * 0.11 + wp.y * 0.07) * 0.6 +
                 sin(t * 2.1 + wp.x * 0.05 - wp.y * 0.13) * 0.4;
    // m: 3 cm per meter of height, as the game's wind ties (Tie3::render_tree_wind) lean
    float bend = 0.03 * sway_h;
    float k = tune_tie_wind * enh_tie_wind.y * 4096.0;
    world.xz += vec2(0.8, 0.6) * (gust * 0.7 + 0.3) * bend * k;
    if ((sway & 0x8000) != 0) {
      float f = min(sway_h * 0.5, 1.0) * 0.035 * k;  // up to 3.5 cm
      world += vec3(sin(t * 6.1 + wp.x * 1.7 + wp.y * 1.3), sin(t * 7.3 + wp.x * 1.1 - wp.y * 1.9) * 0.6,
                    sin(t * 5.3 - wp.x * 1.4 + wp.y * 1.6)) * f;
    }
  }

  // Step 3, the camera transform
  vec3 vert = world - cam_trans.xyz;
  vec4 transformed = -pc_camera[3];
  transformed.w = 0;
  transformed -= pc_camera[0] * vert.x;
  transformed -= pc_camera[1] * vert.y;
  transformed -= pc_camera[2] * vert.z;

  // do fog!
  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);

  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;

  // time of day lookup
  fragment_color = texelFetch(tex_T10, tod_sway.x, 0);
  // color adjustment
  fragment_color *= 2;
  fragment_color.a *= 2;

  if (decal == 1) {
    // tfrag/tie always use TCC=RGB, so even with decal, alpha comes from fragment.
    fragment_color.xyz = vec3(1.0, 1.0, 1.0);
  }
  if (tune_tie_wind >= 10.0) {
    float hw = 0.15 + 0.85 * sway_h / 12.0;
    fragment_color.xyz = sway == 0 ? fragment_color.xyz * 0.3
                                   : ((sway & 0x8000) != 0 ? vec3(0.0, 0.0, 2.0 * hw) : vec3(0.0, hw, 0.0));
  }

  tex_coord = tex_coord_in;
  enh_normal = normal_in.xyz;
  enh_pos = vert;
  enh_lod_kind = normal_in.w > 0.5 ? 1 : 0;
}
