#version 410 core

// Enhanced: a detailed model's far picture (impostor, EnhancedScenery), one quad per copy. Its
// atlas holds the model seen from a grid of directions over the upper hemisphere (hemi-octahedral,
// tools/blender/grass_variants.py). The quad faces the camera, in the copy's own space (so placed,
// turned and scaled as the model), and shows the three pictures taken nearest the view, blended
// by how near: turning around it, no picture snaps to the next. Each picture is read where the
// quad's point falls in it (an orthographic view: a plain projection on its camera's plane).
// Swayed by the wind as the models; lit, fogged and colored like shrub_inst.vert.

layout (location = 0) in vec2 corner_in;  // -1..1
layout (location = 3) in vec4 inst_c0;  // the copy's model-to-world columns; c0.w: its time of day color
layout (location = 4) in vec4 inst_c1;  // c1.w, c2.w: a swap (enh_xfade)
layout (location = 5) in vec4 inst_c2;
layout (location = 6) in vec4 inst_c3;

uniform vec4 hvdf_offset;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform sampler1D tex_T10;
uniform vec3 enh_base_rgb;
uniform vec4 enh_imp;        // the model's bounds' center (its own space) and half their diagonal
uniform float enh_imp_grid;  // pictures per side
uniform vec2 enh_tip;        // the model's foot and height, its own space (the wind sways the top)
uniform float enh_wind;
uniform float enh_time;

out vec4 fragment_color;
out float fogginess;
out vec3 enh_pos;
out vec2 enh_imp_local[3];       // the point in each of the three pictures (-1..1 inside it)
flat out vec2 enh_imp_cell[3];   // which pictures
flat out vec3 enh_imp_weight;
flat out mat3 enh_imp_turn;      // model to world (normals)
flat out vec3 enh_imp_view;      // toward the camera, the model's space
flat out vec2 enh_xfade;

// the view direction (y up) at the center of a hemi-octahedral cell (uv 0..1), as the bake
vec3 hemi_oct_dir(vec2 uv) {
  vec2 p = uv * 2.0 - 1.0;
  float x = (p.x + p.y) * 0.5, z = (p.x - p.y) * 0.5;
  return normalize(vec3(x, 1.0 - abs(x) - abs(z), z));
}

// a camera's right and up looking along f (as the bake's)
void frame(vec3 f, out vec3 right, out vec3 up) {
  right = normalize(cross(f, vec3(0.0, 1.0, 0.0)));
  up = cross(right, f);
}

void main() {
  mat3 turn = mat3(inst_c0.xyz, inst_c1.xyz, inst_c2.xyz);
  // the camera seen from the copy, in the model's own space
  vec3 to_cam = normalize(inverse(turn) * (cam_trans.xyz - inst_c3.xyz) - enh_imp.xyz);
  vec3 v = to_cam;
  v.y = max(v.y, 0.0);
  v /= abs(v.x) + abs(v.y) + abs(v.z);
  vec2 uv = vec2(v.x + v.z, v.x - v.z) * 0.5 + 0.5;
  // the three picture centers around it (cell centers sit at (i + 0.5) / N) and their weights
  vec2 g = clamp(uv * enh_imp_grid - 0.5, vec2(0.0), vec2(enh_imp_grid - 1.0));
  vec2 i0 = min(floor(g), vec2(enh_imp_grid - 2.0));
  vec2 fr = g - i0;
  if (fr.x + fr.y < 1.0) {
    enh_imp_cell[0] = i0;
    enh_imp_cell[1] = i0 + vec2(1.0, 0.0);
    enh_imp_cell[2] = i0 + vec2(0.0, 1.0);
    enh_imp_weight = vec3(1.0 - fr.x - fr.y, fr.x, fr.y);
  } else {
    enh_imp_cell[0] = i0 + vec2(1.0, 1.0);
    enh_imp_cell[1] = i0 + vec2(0.0, 1.0);
    enh_imp_cell[2] = i0 + vec2(1.0, 0.0);
    enh_imp_weight = vec3(fr.x + fr.y - 1.0, 1.0 - fr.x, 1.0 - fr.y);
  }

  // the quad, facing the camera
  vec3 right, up;
  frame(-to_cam, right, up);
  vec3 offset = (right * corner_in.x + up * corner_in.y) * enh_imp.w;
  for (int k = 0; k < 3; k++) {
    vec3 r, u;
    frame(-hemi_oct_dir((enh_imp_cell[k] + 0.5) / enh_imp_grid), r, u);
    enh_imp_local[k] = vec2(dot(offset, r), dot(offset, u)) / enh_imp.w;
  }
  vec3 local = enh_imp.xyz + offset;
  vec3 world = turn * local + inst_c3.xyz;
  // the wind on its top half, as shrub_inst.vert's
  float tip = clamp((local.y - enh_tip.x) / enh_tip.y, 0.0, 1.0);
  if (enh_wind > 0.0) {
    vec2 wp = inst_c3.xz / 4096.0;
    float gust = sin(enh_time * 1.3 + wp.x * 0.11 + wp.y * 0.07) * 0.6 +
                 sin(enh_time * 2.1 + wp.x * 0.05 - wp.y * 0.13) * 0.4;
    world.xz += vec2(0.8, 0.6) * gust * enh_wind * tip * tip * 0.12 * 4096.0;
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
  enh_pos = vert;
  enh_imp_turn = turn;
  enh_imp_view = to_cam;
  enh_xfade = vec2(inst_c1.w, inst_c2.w);
}
