#version 410 core

// Enhanced: a private level's own ground: sand, grass, rock, dirt as painted, rock on what's too steep
// to hold anything else; each its grain at two scales, so that it doesn't show as tiles.

layout (location = 0) out vec4 color;
layout (location = 1) out vec4 enh_normal_out;

in vec4 fragment_color;
in float fogginess;
in vec3 enh_normal;
in vec3 enh_pos;
in vec3 enh_world;
in vec3 enh_mask;

uniform sampler2D tex_mat0;
uniform sampler2D tex_mat1;
uniform sampler2D tex_mat2;
uniform sampler2D tex_mat3;
uniform vec4 enh_luma;  // each one's average brightness: only its grain is used
uniform vec4 fog_color;
uniform float enh_water;  // 1: a lake's surface
uniform float enh_time;
uniform vec3 enh_sun;  // toward the sun

float grain(sampler2D t, vec2 p, float luma) {
  float a = dot(texture(t, p / 3.1).rgb, vec3(0.3, 0.59, 0.11));
  float b = dot(texture(t, p.yx / 11.7 + 0.37).rgb, vec3(0.3, 0.59, 0.11));
  return clamp(a * b / (luma * luma), 0.3, 1.8);
}

float grain_steep(sampler2D t, vec3 n, float luma) {
  // (projected the way the slope faces: a cliff's texture isn't stretched down it)
  vec3 w = pow(abs(n), vec3(4.0));
  w /= w.x + w.y + w.z;
  return grain(t, enh_world.zy, luma) * w.x + grain(t, enh_world.xz, luma) * w.y +
         grain(t, enh_world.xy, luma) * w.z;
}

void main() {
  if (enh_water > 0.5) {
    // small moving ripples, the sky at a grazing look, deep color straight down
    vec2 p = enh_world.xz;
    vec2 d = vec2(sin(p.x * 1.3 + enh_time * 1.1) + sin(p.y * 0.7 + p.x * 0.4 - enh_time * 0.8),
                  sin(p.y * 1.1 - enh_time * 0.9) + sin(p.x * 0.6 - p.y * 0.5 + enh_time * 0.7));
    vec3 w = normalize(vec3(d.x * 0.03, 1.0, d.y * 0.03));
    float fres = pow(1.0 - max(dot(normalize(-enh_pos), w), 0.0), 4.0);
    vec3 c = mix(vec3(0.05, 0.19, 0.24), vec3(0.50, 0.66, 0.80), 0.12 + 0.75 * fres);
    color = vec4(fragment_color.rgb * c * 1.6, 0.72 + 0.25 * fres);
    color.xyz = mix(color.xyz, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
    enh_normal_out = vec4(0.0);
    return;
  }
  vec3 n = normalize(enh_normal);
  const vec3 sand = vec3(0.70, 0.60, 0.46), grass = vec3(0.27, 0.36, 0.13),
             rock = vec3(0.42, 0.34, 0.28), dirt = vec3(0.38, 0.27, 0.18);
  float steep = 1.0 - smoothstep(0.6, 0.78, n.y);
  float k_grass = enh_mask.x * (1.0 - steep);
  float k_dirt = enh_mask.z * (1.0 - steep);
  float k_rock = clamp(enh_mask.y + steep, 0.0, 1.0);
  float k_sand = max(1.0 - k_grass - k_dirt - k_rock, 0.0);
  float sum = k_sand + k_grass + k_rock + k_dirt;
  vec3 c = vec3(0.0);
  if (k_sand > 0.01) c += sand * grain(tex_mat0, enh_world.xz, enh_luma.x) * k_sand;
  if (k_grass > 0.01) c += grass * grain(tex_mat1, enh_world.xz, enh_luma.y) * k_grass;
  if (k_rock > 0.01) c += rock * mix(1.0, grain_steep(tex_mat2, n, enh_luma.z), 0.6) * k_rock;
  if (k_dirt > 0.01) c += dirt * grain(tex_mat3, enh_world.xz, enh_luma.w) * k_dirt;
  c /= max(sum, 0.001);
  // the shape of the land: the light on its slopes, against that on the flat
  float flat_light = 0.45 + 0.75 * max(enh_sun.y, 0.0);
  c *= clamp((0.45 + 0.75 * max(dot(n, enh_sun), 0.0)) / flat_light, 0.35, 1.5);

  enh_normal_out = vec4((k_rock > 0.5 ? 0.75 : 1.0) * n * 0.5 + 0.5, 1.0);
  color = vec4(fragment_color.rgb * c, fragment_color.a);
  color.xyz = mix(color.xyz, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
