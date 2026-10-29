#version 410 core

// Enhanced: a detailed model standing in for a shrub proto near the camera, drawn instanced
// (EnhancedScenery). Lit, fogged and placed like shrub.vert; shrub.frag colors it.

layout (location = 0) in vec3 position_in;  // the model's own space, game units
layout (location = 1) in vec2 tex_coord_in;
layout (location = 2) in vec4 normal_in;
layout (location = 3) in vec4 inst_c0;  // the copy's model-to-world columns; c0.w: its time of day color
layout (location = 4) in vec4 inst_c1;  // c1.w, c2.w: a swap (shrub.frag's enh_xfade)
layout (location = 5) in vec4 inst_c2;
layout (location = 6) in vec4 inst_c3;

uniform vec4 hvdf_offset;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform sampler1D tex_T10;
uniform vec3 enh_base_rgb;  // the proto's own vertex color (baked light), averaged
uniform vec2 enh_tip;       // the model's foot and height, its own space (wind and push sway the top)
uniform float enh_wind;
uniform float enh_time;
uniform float enh_foliage;
uniform vec4 enh_player;
uniform vec4 enh_pushers[8];
uniform int enh_pusher_count;
uniform sampler2D tex_T2;  // the crush map (EnhancedScenery)
uniform vec4 enh_crush;    // its corner (world x, z), its size (game units), on

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
out vec3 enh_normal;
out vec3 enh_pos;
flat out int enh_lod_kind;
flat out vec2 enh_xfade;

void main() {
  mat3 turn = mat3(inst_c0.xyz, inst_c1.xyz, inst_c2.xyz);
  vec3 world = turn * position_in + inst_c3.xyz;
  float tip = clamp((position_in.y - enh_tip.x) / enh_tip.y, 0.0, 1.0);
  if (enh_wind > 0.0) {
    vec2 wp = inst_c3.xz / 4096.0;
    float gust = sin(enh_time * 1.3 + wp.x * 0.11 + wp.y * 0.07) * 0.6 +
                 sin(enh_time * 2.1 + wp.x * 0.05 - wp.y * 0.13) * 0.4;
    float flutter = sin(enh_time * 7.0 + wp.x * 1.7 + wp.y * 1.3) * 0.25;
    world.xz += vec2(0.8, 0.6) * (gust + flutter) * enh_wind * tip * tip * 0.12 * 4096.0;
  }
  if (enh_foliage > 0.0 && enh_player.w > 0.0) {
    // (each blade by where its own foot is, not the tuft's middle: in a wide clump only the side
    // walked through lies down)
    vec3 foot = turn * vec3(position_in.x, enh_tip.x, position_in.z) + inst_c3.xyz;
    vec2 offset = vec2(0.0);
    float press = 0.0;
    for (int i = -1; i < enh_pusher_count; i++) {
      vec4 pusher = i < 0 ? enh_player : enh_pushers[i];
      vec3 d = foot - pusher.xyz;
      float hd = length(d.xz);
      float near = 1.0 - smoothstep(pusher.w * 0.25, pusher.w, hd);
      near *= 1.0 - smoothstep(1.5 * 4096.0, 3.0 * 4096.0, abs(d.y));
      offset += (hd > 1.0 ? d.xz / hd : vec2(1.0, 0.0)) * near;
      press = max(press, near);
    }
    float len = length(offset);
    offset = len > 1.0 ? offset / len : offset;
    // where someone went through a little while ago the grass still lies (the crush map,
    // EnhancedScenery): bent away from their path, rising back over a few seconds
    if (enh_crush.w > 0.0) {
      vec2 cuv = (foot.xz - enh_crush.xy) / enh_crush.z;
      if (all(greaterThan(cuv, vec2(0.0))) && all(lessThan(cuv, vec2(1.0)))) {
        vec3 c = texture(tex_T2, cuv).xyz;  // the bend (direction x amount), the height it was made at (m)
        float lie = length(c.xy) * (1.0 - smoothstep(1.5, 3.0, abs(c.z - foot.y / 4096.0)));
        if (lie > press) {
          offset = c.xy / max(length(c.xy), 1e-4) * lie;
          press = lie;
        }
      }
    }
    // (by how tall the tuft is: a tall bush lies down as far as a short tuft, its tips near the
    // ground)
    float tall = enh_tip.y * length(inst_c1.xyz);
    float push = tip * tip;
    world.xz += offset * push * 0.75 * tall;
    world.y -= press * push * 0.6 * tall;
  }

  vec3 vert = world - cam_trans.xyz;
  vec4 transformed = -pc_camera[3];
  transformed -= pc_camera[0] * vert.x;
  transformed -= pc_camera[1] * vert.y;
  transformed -= pc_camera[2] * vert.z;
  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;

  vec4 tod_color = texelFetch(tex_T10, int(inst_c0.w), 0);
  fragment_color = vec4(enh_base_rgb, 0.5) * tod_color * 4;

  tex_coord = vec3(tex_coord_in, 1.0);
  enh_normal = normalize(turn * normal_in.xyz);
  enh_pos = vert;
  enh_lod_kind = 2;
  enh_xfade = vec2(inst_c1.w, inst_c2.w);  // a swap to the next version (EnhancedScenery)
}
