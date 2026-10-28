#version 410 core

// Enhanced: a detailed model's far picture (impostor, EnhancedScenery), one quad per copy. Its
// atlas holds the model seen from a grid of directions over the upper hemisphere (hemi-octahedral,
// tools/blender/grass_variants.py); the quad shows the cell nearest the view, turned as that
// picture's camera was, in the copy's own space (so placed, turned and scaled as the model).
// Lit, fogged and colored like shrub_inst.vert; shrub.frag colors it.

layout (location = 0) in vec2 corner_in;  // -1..1
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
uniform vec3 enh_base_rgb;
uniform vec4 enh_imp;      // the model's bounds' center (its own space) and half their diagonal
uniform float enh_imp_grid;  // cells per side

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
out vec3 enh_normal;
out vec3 enh_pos;
flat out int enh_lod_kind;
flat out vec2 enh_xfade;

// the view direction (y up) at the center of a hemi-octahedral cell (uv 0..1), as the bake
vec3 hemi_oct_dir(vec2 uv) {
  vec2 p = uv * 2.0 - 1.0;
  float x = (p.x + p.y) * 0.5, z = (p.x - p.y) * 0.5;
  return normalize(vec3(x, 1.0 - abs(x) - abs(z), z));
}

void main() {
  mat3 turn = mat3(inst_c0.xyz, inst_c1.xyz, inst_c2.xyz);
  // the camera seen from the copy, in the model's own space
  vec3 to_cam = inverse(turn) * (cam_trans.xyz - inst_c3.xyz) - enh_imp.xyz;
  vec3 d = normalize(to_cam);
  d.y = max(d.y, 0.0);
  d /= abs(d.x) + abs(d.y) + abs(d.z);
  vec2 uv = vec2(d.x + d.z, d.x - d.z) * 0.5 + 0.5;
  vec2 cell = clamp(floor(uv * enh_imp_grid), 0.0, enh_imp_grid - 1.0);
  // that cell's camera: looking along f, right = f x up, up = right x f
  vec3 f = -hemi_oct_dir((cell + 0.5) / enh_imp_grid);
  vec3 right = normalize(cross(f, vec3(0.0, 1.0, 0.0)));
  vec3 up = cross(right, f);
  vec3 local = enh_imp.xyz + (right * corner_in.x + up * corner_in.y) * enh_imp.w;
  vec3 world = turn * local + inst_c3.xyz;

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

  // the atlas cell (the png loads top row first: v runs down from the bake's bottom-up rows)
  vec2 in_cell = corner_in * 0.5 + 0.5;
  vec2 t = (cell + in_cell) / enh_imp_grid;
  tex_coord = vec3(t.x, 1.0 - t.y, 1.0);
  // lit as a tuft: mostly from above, a little toward the camera
  enh_normal = normalize(turn * normalize(vec3(0.0, 0.8, 0.0) - f * 0.4));
  enh_pos = vert;
  enh_lod_kind = 3;
  enh_xfade = vec2(inst_c1.w, inst_c2.w);
}
