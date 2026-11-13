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
// the trunk's lean, m per meter of height and unit of wind (0.03 = the old, whole-tree lean)
uniform float tune_tie_trunk = 0.006;
// a leaf's swing, m per meter of reach from its trunk and unit of wind (0 = the old: flutter only)
uniform float tune_tie_frond = 0.10;
// and how much more per unit of wind past 1 (0 = linear, as before the weather's wind amount)
uniform float tune_tie_frond_gale = 0.6;
// below this wind (+ 0.4 to fade in) the trees stand still: the weather's calm (0 = sway at any wind)
uniform float tune_tie_calm = 0.3;

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
out vec3 enh_normal;
out vec3 enh_pos;
flat out int enh_lod_kind;  // Enhanced: 1 = a detailed model stands in near the camera (TIE only)
out float enh_leaf;  // Enhanced: 1 on a TIE tree's leaves (their flutter flag), for the post's backlight

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
  bool leaf = (sway & 0x8000) != 0;
  // meters above the plant's foot; a leaf's reach: meters from its trunk (EnhancedTieWind.h)
  float sway_h = leaf ? float((sway >> 8) & 0x7f) * 0.1 : float(sway & 0x7fff) * (12.0 / 32767.0);
  float reach = leaf ? float(sway & 0xff) * 0.025 : 0.0;
  // (>= 20: sway at tune_tie_wind - 19 and paint how far each vertex moves, red = 30 cm)
  float tie_amp = tune_tie_wind >= 20.0 ? tune_tie_wind - 19.0 : tune_tie_wind;
  if (sway != 0 && tie_amp > 0.0 && (tune_tie_wind < 10.0 || tune_tie_wind >= 20.0)) {
    float t = enh_tie_wind.x;
    vec2 wp = position_in.xz / 4096.0;
    float gust = sin(t * 1.3 + wp.x * 0.11 + wp.y * 0.07) * 0.6 +
                 sin(t * 2.1 + wp.x * 0.05 - wp.y * 0.13) * 0.4;
    // (a still day, wind now under tune_tie_calm + 0.4, fades to nearly still: calm barely moves,
    // a breeze (clear weather, ~0.7+) sways as before)
    float calm = clamp((enh_tie_wind.y - tune_tie_calm) / 0.4, 0.0, 1.0);
    float k = tie_amp * enh_tie_wind.y * calm * 4096.0;
    // the trunk barely leans (tune_tie_trunk m per meter of height: 1 cm at 10 m in a calm, ~5 in a
    // sandstorm), its leaves with it so nothing tears
    world.xz += vec2(0.8, 0.6) * (gust * 0.7 + 0.3) * tune_tie_trunk * sway_h * k;
    if (leaf) {
      // the fronds swing from where they grow (tune_tie_frond m per meter of reach): pushed
      // downwind and bobbing, each plant a little out of step, the tips most
      float ph = wp.x * 0.37 + wp.y * 0.29;
      float push = sin(t * 1.7 + ph) * 0.5 + sin(t * 2.9 + ph * 1.7) * 0.3 + 0.35;
      float bob = sin(t * 2.3 + ph * 1.3) * 0.6 + sin(t * 3.7 - ph) * 0.4;
      // (a strong wind bends the blades more than in proportion: past a breeze (wind now 1) each
      // unit adds tune_tie_frond_gale more swing, so windy weather and a sandstorm read as such
      // while a calm or clear day stays as it was)
      float gale = 1.0 + tune_tie_frond_gale * max(enh_tie_wind.y - 1.0, 0.0);
      float r = reach * tune_tie_frond * k * gale;
      world.xz += vec2(0.8, 0.6) * push * r;
      world.y += bob * 0.6 * r;
      // and a flutter on top, up to 3.5 cm
      float f = min(reach * 0.5, 1.0) * 0.035 * k;
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
  if (tune_tie_wind >= 20.0) {
    float moved = length(world - position_in) / (0.3 * 4096.0);
    fragment_color.xyz = sway == 0 ? fragment_color.xyz * 0.3
                                   : vec3(min(moved, 1.0) * 2.0, leaf ? 0.15 : 0.5, 0.0);
  } else if (tune_tie_wind >= 10.0) {
    // (leaves blue by their reach, 0..4 m; trunks green by height)
    float hw = 0.15 + 0.85 * sway_h / 12.0;
    fragment_color.xyz = sway == 0 ? fragment_color.xyz * 0.3
                                   : (leaf ? vec3(0.0, 0.0, 0.3 + 1.7 * min(reach / 4.0, 1.0))
                                           : vec3(0.0, hw, 0.0));
  }

  tex_coord = tex_coord_in;
  enh_normal = normal_in.xyz;
  enh_pos = vert;
  enh_lod_kind = normal_in.w > 0.5 ? 1 : 0;
  enh_leaf = (tod_sway.y & 0x8000) != 0 ? 1.0 : 0.0;
}
