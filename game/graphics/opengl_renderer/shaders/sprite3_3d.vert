#version 410 core

layout (location = 0) in vec4 xyz_sx;
layout (location = 1) in vec4 quat_sy;
layout (location = 2) in vec4 rgba;
layout (location = 3) in uvec2 flags_matrix;
layout (location = 4) in uvec4 tex_info_in;

uniform vec4 hvdf_offset;
uniform mat4 camera;
uniform mat4 hud_matrix;
uniform vec4 hud_hvdf_offset;
uniform vec4 hud_hvdf_user[75];
uniform float pfog0;
uniform float fog_min;
uniform float fog_max;
uniform float min_scale;
uniform float max_scale;
uniform float deg_to_rad;
uniform float inv_area;
uniform vec4 basis_x;
uniform vec4 basis_y;
uniform vec4 xy_array[8];
uniform vec4 xyz_array[4];
uniform vec4 st_array[4];

uniform int enh_light_count;     // Enhanced: lights near the smoke (world, radius), 0 = unlit
uniform vec4 enh_light_pos[8];
uniform vec4 enh_light_color[8];
uniform float enh_smoke;         // how much they brighten and tint it
uniform float tune_smoke_soot = 1.0;      // Enhanced: lit smoke tinted, not burnt white (0: off)
uniform float tune_smoke_soot_cap = 1.3;  // how much brighter the light may make a puff
uniform int enh_soot_count;      // Enhanced: the last explosions (world, reach), 0 = none
uniform vec4 enh_soot_pos[4];
uniform float enh_soot_age[4];   // 0..1 over the soot's life
uniform float tune_soot_dark = 0.45;   // an explosion's grey puffs taken to this much of their color
uniform float tune_soot_thin = 0.7;    // and of their opacity, fading out over the soot's life
uniform int enh_soot_add;              // this draw is additive (an explosion's flash and fire sprites)
uniform float tune_soot_flash = 1.0;   // their sum kept orange, not clipped white (0: as the game has it)
uniform int enh_fx_class;        // Enhanced: this draw's smoke or flames (EnhancedParticles), 0 = no

flat out vec4 fragment_color;
out vec3 tex_coord;
flat out uvec2 tex_info;
flat out int enh_kind;           // 0 untouched, 1 smoke, 2 a blended flame, 3 an added flame
out vec2 enh_q;                  // where in the sprite: -1..1 along the screen, not turning with it
flat out vec4 enh_cen;           // the sprite's place in the noise; w: its size (game units)

vec4 matrix_transform(mat4 mtx, vec3 pt) {
  return mtx[3]
       + mtx[0] * pt.x
       + mtx[1] * pt.y
       + mtx[2] * pt.z;
}

mat3 sprite_quat_to_rot(vec3 quat) {
  mat3 result;
  float qr = sqrt(abs(1.0 - (quat.x * quat.x + quat.y * quat.y + quat.z * quat.z)));
  result[0][0] = 1.0 - 2.0 * (quat.y * quat.y + quat.z * quat.z);
  result[1][0] = 2.0 * (quat.x * quat.y - quat.z * qr);
  result[2][0] = 2.0 * (quat.x * quat.z + quat.y * qr);
  result[0][1] = 2.0 * (quat.x * quat.y + quat.z * qr);
  result[1][1] = 1.0 - 2.0 * (quat.x * quat.x + quat.z * quat.z);
  result[2][1] = 2.0 * (quat.y * quat.z - quat.x * qr);
  result[0][2] = 2.0 * (quat.x * quat.z - quat.y * qr);
  result[1][2] = 2.0 * (quat.y * quat.z + quat.x * qr);
  result[2][2] = 1.0 - 2.0 * (quat.x * quat.x + quat.y * quat.y);
  return result;
}

vec4 sprite_transform2(vec3 root, vec4 off, mat3 sprite_rot, float sx, float sy) {
  vec3 pos = root;

  vec3 offset = sprite_rot[0] * off.x * sx + sprite_rot[1] * off.y + sprite_rot[2] * off.z * sy;

  pos += offset;
  vec4 transformed_pos = -matrix_transform(camera, pos);
  float Q = pfog0 / transformed_pos.w;
  transformed_pos.xyz *= Q;
  transformed_pos.xyz += hvdf_offset.xyz;

  return transformed_pos;
}

// Enhanced: smoke or flame, by the class of its texture and its own color (a warm flame, grey smoke:
// not the eco and magic that use the same pictures)
int enh_kind_of(vec4 col) {
  float mx = max(col.r, max(col.g, col.b));
  float sat = (mx - min(col.r, min(col.g, col.b))) / max(mx, 1e-3);
  bool warm = col.r > col.g && col.g > col.b && sat > 0.4;
  if (enh_fx_class == 1) {
    return warm ? 2 : (sat < 0.45 ? 1 : 0);
  }
  if (enh_fx_class == 2) {
    return 3;
  }
  return enh_fx_class == 3 && warm ? 3 : 0;
}

void main() {
  enh_kind = 0;
  enh_q = vec2(0.0);
  enh_cen = vec4(0.0);

  // STEP 1: UNPACK DATA AND CREATE READABLE VARIABLES

  vec3 position = xyz_sx.xyz;
  float sx = xyz_sx.w;
  float sy = quat_sy.w;
  fragment_color = rgba;
  uint vert_id = tex_info_in.z;
  uint rendermode = tex_info_in.w; // 2D, HUD, 3D
  vec3 quat = quat_sy.xyz;
  uint matrix = flags_matrix.y;

  vec4 transformed;

  // STEP 2: perspective transform for distance
  vec4 transformed_pos_vf02 = matrix_transform(rendermode == 2 ? hud_matrix : camera, position);
  float Q = pfog0 / transformed_pos_vf02.w;


  // STEP 3: fade out sprite!
  vec4 scales_vf01 = xyz_sx;  // now used for something else.
  scales_vf01.z = sy;  // start building the scale vector
  scales_vf01.zw *= Q;  // sy sx
  scales_vf01.x = scales_vf01.z;  // = sy
  scales_vf01.x *= scales_vf01.w;  // x = sx * sy
  scales_vf01.x *= inv_area;  // x = sx * sy * inv_area (area ratio)
  // Enhanced: not the 3D (turned) sprites: long and thin, the gun beams faded almost to nothing
  if (rendermode != 3u) {
    fragment_color.w *= min(scales_vf01.x, 1.0);  // is this right? doesn't this stall??
  }


  // STEP 4: actual vertex transformation
  if (rendermode == 3) { // 3D sprites

    mat3 rot = sprite_quat_to_rot(quat);
    transformed = sprite_transform2(position, xyz_array[vert_id], rot, sx, sy);

  } else if (rendermode == 1) { // 2D sprites

    transformed_pos_vf02.xyz *= Q;
    vec4 offset_pos_vf10 = transformed_pos_vf02 + hvdf_offset;
    offset_pos_vf10.w = max(offset_pos_vf10.w, fog_min);

    /* transformed_pos_vf02.w = offset_pos_vf10.w - fog_max;
    int fge = matrix == 0;
    if (transformed_pos_vf02.w != 0) {
      fge = false;
    } */

    scales_vf01.z = clamp(scales_vf01.z, min_scale, max_scale);
    scales_vf01.w = clamp(scales_vf01.w, min_scale, max_scale);

    quat.z *= deg_to_rad;
    float sp_sin = sin(quat.z);
    float sp_cos = cos(quat.z);

    vec4 xy0_vf19 = xy_array[vert_id + (flags_matrix.x & 15u)];
    vec4 vf12_rotated = (basis_x * sp_cos) - (basis_y * sp_sin);
    vec4 vf13_rotated_trans = (basis_x * sp_sin) + (basis_y * sp_cos);

    vf12_rotated *= scales_vf01.w;
    vf13_rotated_trans *= scales_vf01.z;

    transformed = offset_pos_vf10 + vf12_rotated * xy0_vf19.x + vf13_rotated_trans * xy0_vf19.y;

    if (enh_fx_class != 0) {
      enh_kind = enh_kind_of(rgba);
      // the corner on screen, without the sprite's turn (y up, as the framebuffer sees it)
      vec2 off = vf12_rotated.xy * xy0_vf19.x + vf13_rotated_trans.xy * xy0_vf19.y;
      float half_size = max(abs(scales_vf01.w), abs(scales_vf01.z)) *
                        max(max(abs(xy0_vf19.x), abs(xy0_vf19.y)), 1e-3);
      enh_q = vec2(off.x / max(abs(basis_x.x), 1e-5), -off.y / max(abs(basis_y.y), 1e-5)) /
              max(half_size, 1e-3);
      // (a puff that moves drifts through the noise: every 6 m a whole tile)
      enh_cen = vec4(fract(position / (6.0 * 4096.0)), max(sx, sy));
    }

  } else if (rendermode == 2) { // hud sprites
  
    transformed_pos_vf02.xyz *= Q;
    vec4 offset_pos_vf10 = transformed_pos_vf02 + (matrix == 0 ? hud_hvdf_offset : hud_hvdf_user[matrix - 1]);

    // NOTE: no max scale for hud
    scales_vf01.z = max(scales_vf01.z, min_scale);
    scales_vf01.w = max(scales_vf01.w, min_scale);

    quat.z *= deg_to_rad;
    float sp_sin = sin(quat.z);
    float sp_cos = cos(quat.z);

    vec4 xy0_vf19 = xy_array[vert_id + (flags_matrix.x & 15u)];
    vec4 vf12_rotated = (basis_x * sp_cos) - (basis_y * sp_sin);
    vec4 vf13_rotated_trans = (basis_x * sp_sin) + (basis_y * sp_cos);

    vf12_rotated *= scales_vf01.w;
    vf13_rotated_trans *= scales_vf01.z;

    transformed = offset_pos_vf10 + vf12_rotated * xy0_vf19.x + vf13_rotated_trans * xy0_vf19.y;

  }

  tex_coord = st_array[vert_id].xyz;


  // STEP 5: final adjustments
  // correct xy offset
  transformed.xy -= (2048.);
  // correct z scale
  transformed.z /= (8388608);
  transformed.z -= 1;
  // correct xy scale
  transformed.x /= (256);
  transformed.y /= -(128);
  // hack
  transformed.xyz *= transformed.w;
  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;
  // Enhanced: behind the camera (the sun, looking down from the editor): not mirrored onto the screen
  // (w is negative in front, as pfog0, for the 2D sprites too: by the sign of Q, or every gun beam and
  // every particle in front of the camera was dropped, the arena's light orbs among them)
  if (rendermode != 2u && Q <= 0.0) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  }

  if (enh_soot_count > 0 && enh_soot_add != 0 && rendermode != 2u) {
    // Enhanced: an explosion's additive sprites (its white flash, its glows) summed past full to a
    // white ball with rays over the sand by day and the dark by night: kept in the fire's hue, the
    // white ones toward orange-yellow and a little dimmer, early in the blast only
    float w = 0.0;
    for (int i = 0; i < enh_soot_count; i++) {
      float k = 1.0 - smoothstep(0.55, 1.0, length(position - enh_soot_pos[i].xyz) / enh_soot_pos[i].w);
      w = max(w, k);
    }
    w *= tune_soot_flash;
    fragment_color.rgb *= mix(vec3(1.0), vec3(0.85, 0.52, 0.2), w);
  } else if (enh_soot_count > 0 && rendermode != 2u) {
    // Enhanced: an explosion's grey puffs, pale over noon sand and lit to white by its flash, are
    // soot: darker, a little warm, thinner, and gone sooner (look.blast_punch 0: as the game has them)
    float mx = max(fragment_color.r, max(fragment_color.g, fragment_color.b));
    float sat = (mx - min(fragment_color.r, min(fragment_color.g, fragment_color.b))) / max(mx, 1e-3);
    float w = 0.0;
    float fade = 0.0;
    for (int i = 0; i < enh_soot_count; i++) {
      vec3 d = position - enh_soot_pos[i].xyz;
      float r = enh_soot_pos[i].w;
      float k = 1.0 - smoothstep(0.55, 1.0, length(d) / r);
      w = max(w, k);
      fade = max(fade, k * smoothstep(0.3, 1.0, enh_soot_age[i]));
    }
    w *= 1.0 - smoothstep(0.25, 0.5, sat);  // grey smoke only, not the flames
    fade *= 1.0 - smoothstep(0.25, 0.5, sat);
    fragment_color.rgb *= mix(vec3(1.0), vec3(1.0, 0.9, 0.8) * tune_soot_dark, w);
    fragment_color.a *= mix(1.0, tune_soot_thin, w) * (1.0 - fade);
  }

  if (enh_light_count > 0 && rendermode != 2u) {
    // Enhanced: a puff near a lamp or a fire takes its color, by the falloff the post pass uses
    vec3 added = vec3(0.0);
    for (int i = 0; i < enh_light_count; i++) {
      vec3 d = position - enh_light_pos[i].xyz;
      float r = enh_light_pos[i].w;
      float k = max(1.0 - dot(d, d) / (r * r), 0.0);
      // (not the fire's own flames, at its heart: lit by it too, they burned past full, pale and
      // tinted by the lamp's color)
      k *= smoothstep(0.06, 0.16, length(d) / r);
      added += enh_light_color[i].rgb * (k * k);
    }
    // (the smoke, grey: not the fire's own flames, strongly colored, which it lit too, burning them
    // past full to a pale yellow-green)
    float mx = max(fragment_color.r, max(fragment_color.g, fragment_color.b));
    float sat = (mx - min(fragment_color.r, min(fragment_color.g, fragment_color.b))) / max(mx, 1e-3);
    vec3 lit = fragment_color.rgb * (1.0 + min(added, vec3(2.0)) * enh_smoke * (1.0 - smoothstep(0.2, 0.45, sat)));
    // (an explosion's flash, the frame's strongest light, took its own grey smoke to 2.6x: a milky
    // white cloud by day and by night. The light tints the smoke and brightens it at most by
    // tune_smoke_soot_cap; past that only the hue moves toward the light's. 0: as before)
    if (tune_smoke_soot > 0.0) {
      const vec3 lw = vec3(0.3, 0.59, 0.11);
      float l0 = dot(fragment_color.rgb, lw);
      float l1 = dot(lit, lw);
      float cap = l0 * tune_smoke_soot_cap;
      lit *= mix(1.0, min(1.0, cap / max(l1, 1e-4)), tune_smoke_soot);
    }
    fragment_color.rgb = lit;
  }

  fragment_color *= 2;
  fragment_color.w *= 2;

  tex_info = tex_info_in.xy;
}
