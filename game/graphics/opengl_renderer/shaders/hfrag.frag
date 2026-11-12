#version 410 core

layout (location = 0) out vec4 color;
layout (location = 1) out vec4 enh_normal_out;  // Enhanced: world normal, a = 0 for none
layout (location = 2) out vec4 enh_albedo_out;  // Enhanced: texture color before the baked light, a: that light / 2 (0: none)
in vec3 enh_normal;
in vec3 enh_pos;

// Enhanced: the detail material (EnhancedDetail.h, detail.json "hfrag") on unit 3, as tfrag3.frag:
// R G its normal along world X and Z, B a roughness offset about 0.5, A AO. The dunes keep their
// colors; it adds the fine relief near the camera and codes a roughness for the composite's GGX
uniform sampler2D tex_T3;
uniform int enh_detail;      // 0 none, 1 on, 2 on and the albedo may carry its roughness (PBR on)
uniform vec4 enh_detail_k;   // 1 / tile (game units), normal, roughness, AO strengths
uniform vec4 cam_trans;      // (hfrag.vert's: enh_pos is relative to it)
uniform float tune_detail = 1.0;                  // shaders.tune_detail: strength, 0 off
uniform vec2 tune_detail_fade = vec2(15.0, 120.0);  // m: full up to, gone by (long, soft fade)
vec4 g_det = vec4(0.0, 0.0, 0.5, 1.0);
float g_det_k = 0.0;
// the broad relief from the dunes' own grain picture (they have no PBR map): its slopes along X, Z
vec2 g_broad = vec2(0.0);
uniform float tune_detail_broad = 0.8;  // shaders.tune_detail_broad: its strength (0 off)
// shaders.tune_detail_hfrag_pbr 1: the dunes' albedo coded with a roughness (the composite's GGX and its
// normal relief instead of the color's slope: without it the fine normal hardly shows). Kept rough
// (DET_SAND_R, detail.json rough 0.3): with roughness down to ~0.5 the open dunes glinted like wet sand
// toward a low sun, out to 300 m. 0 (default): the dunes keep the luminance relief and no highlight
uniform float tune_detail_hfrag_pbr = 0.0;
// without the coding: the fine normal shaded here by the key light (EnhancedDetail.h's sun, w its
// strength), as the composite shades a coded map's bumps against the broad facing
uniform vec4 enh_detail_sun;
uniform float tune_detail_shade = 1.2;  // shaders.tune_detail_shade
vec3 g_n_base = vec3(0.0), g_n_det = vec3(0.0);
const float DET_SHADE_UP = 0.82, DET_SHADE_SIDE = 0.572;  // (sin, cos of 55 degrees)
const float DET_SAND_R = 0.88;  // the dunes' dry sand: rougher than tfrag3.frag's PBR_R[3] (0.8), or it glints like water at a low sun

float enh_detail_noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  vec2 h = vec2(127.1, 311.7);
  float a = fract(sin(dot(i, h)) * 43758.5453);
  float b = fract(sin(dot(i + vec2(1.0, 0.0), h)) * 43758.5453);
  float c = fract(sin(dot(i + vec2(0.0, 1.0), h)) * 43758.5453);
  float d = fract(sin(dot(i + vec2(1.0, 1.0), h)) * 43758.5453);
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// (as tfrag3.frag's: the tile and a turned copy 2.7x as big, slopes summed)
void enh_detail_sample(vec3 dp1, vec3 dp2) {
  if (enh_detail == 0 || tune_detail <= 0.0) {
    return;
  }
  float k = 1.0 - smoothstep(tune_detail_fade.x * 4096.0, tune_detail_fade.y * 4096.0, length(enh_pos));
  if (k <= 0.0) {
    return;
  }
  const mat2 rot = mat2(0.8, -0.6, 0.6, 0.8);
  float s1 = enh_detail_k.x, s2 = enh_detail_k.x / 2.7;
  vec2 uv1 = fract(cam_trans.xz * s1) + enh_pos.xz * s1;
  vec2 uv2 = fract((rot * cam_trans.xz) * s2 + 0.37) + (rot * enh_pos.xz) * s2;
  vec4 a = textureGrad(tex_T3, uv1, dp1.xz * s1, dp2.xz * s1);
  vec4 b = textureGrad(tex_T3, uv2, (rot * dp1.xz) * s2, (rot * dp2.xz) * s2);
  float nz = enh_detail_noise((cam_trans.xz + enh_pos.xz) / (4096.0 * 9.0));
  float wa = mix(0.55, 1.0, nz), wb = mix(0.75, 0.35, nz);
  vec2 na = a.xy * 2.0 - 1.0, nb = b.xy * 2.0 - 1.0;
  vec2 ga = na / max(sqrt(1.0 - dot(na, na)), 0.3);
  vec2 gb = nb / max(sqrt(1.0 - dot(nb, nb)), 0.3);
  vec2 g = ga * wa + (gb * rot) * wb;
  g_det = vec4(g, 0.5 + (a.b - 0.5) * wa + (b.b - 0.5) * wb, mix(1.0, a.a, wa) * mix(1.0, b.a, wb));
  g_det_k = k * tune_detail;
}

vec3 enh_detail_bump(vec3 n) {
  float steep = smoothstep(0.35, 0.7, abs(n.y));
  vec2 g = g_det.xy * (g_det_k * enh_detail_k.y) + g_broad;
  if (steep <= 0.0 || g == vec2(0.0)) {
    return n;
  }
  float k = steep;
  vec3 t = normalize(vec3(1.0, 0.0, 0.0) - n * n.x);
  vec3 b = normalize(vec3(0.0, 0.0, 1.0) - n * n.z);
  float side = n.y < 0.0 ? -1.0 : 1.0;
  vec3 nd = normalize(n + (t * g.x + b * g.y) * (k * side));
  g_n_base = n * side;
  g_n_det = nd * side;
  return nd;
}

// Enhanced: the smooth normal where it stays near the triangle's own face (as tfrag3.frag)
vec4 enh_pack_normal(vec3 n) {
  vec3 f = cross(dFdx(enh_pos), dFdy(enh_pos));
  float flen = length(f);
  n = normalize(n);
  if (flen > 0.0) {
    f /= flen;
    float d = dot(n, f);
    // (blended, not switched: a hard switch drew the lights' highlights as sharp patches)
    n = normalize(mix(f, n * sign(d), smoothstep(0.6, 0.85, abs(d))));
  }
  n = enh_detail_bump(n);
  // (0.9 long: sand, so the composite draws the wind's ripples on the dunes; see tfrag3.frag)
  return vec4(n * 0.9 * 0.5 + 0.5, 1.0);
}

in vec4 fragment_color;
in vec2 tex_coord;
in float fogginess;
in vec2 world_xz;
uniform sampler2D tex_T0;
uniform sampler2D tex_T1;   // Enhanced: seamless sand detail (when enhanced_sand is set)
uniform int enhanced_sand;
uniform float sand_scale;
uniform vec3 sand_mean;     // average color of tex_T1
uniform float sand_detail;  // how much of its detail goes over the game's sand
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;


void main() {
  enh_albedo_out = vec4(0.0);
  vec3 n = enh_normal;
  enh_detail_sample(dFdx(enh_pos), dFdy(enh_pos));
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, tex_coord);
    if (enhanced_sand != 0) {
      // the game's own sand keeps its colors; the seamless picture only adds its grain, at two
      // scales and turned against each other so neither repeat shows
      vec3 a = texture(tex_T1, world_xz * sand_scale).rgb;
      vec3 b = texture(tex_T1, mat2(0.8, -0.6, 0.6, 0.8) * world_xz * sand_scale * 2.7).rgb;
      vec3 grain = clamp((a * 0.55 + b * 0.45) / max(sand_mean, vec3(1e-3)), 0.6, 1.5);
      T0.rgb *= mix(vec3(1.0), grain, sand_detail);
      // (with the albedo coded below, the composite shades the stored normal instead of the color's
      // slope: the picture's shapes go into the normal here, its brightness as height, two texels
      // apart, relative to its mean)
      if (enh_detail == 2 && tune_detail_hfrag_pbr > 0.5 && tune_detail_broad > 0.0) {
        float e = 2.0 / (float(textureSize(tex_T1, 0).x) * sand_scale);
        const vec3 L = vec3(0.3, 0.59, 0.11);
        float hx = dot(texture(tex_T1, (world_xz + vec2(e, 0.0)) * sand_scale).rgb -
                       texture(tex_T1, (world_xz - vec2(e, 0.0)) * sand_scale).rgb, L);
        float hz = dot(texture(tex_T1, (world_xz + vec2(0.0, e)) * sand_scale).rgb -
                       texture(tex_T1, (world_xz - vec2(0.0, e)) * sand_scale).rgb, L);
        g_broad = -vec2(hx, hz) / max(dot(sand_mean, L), 0.05) * tune_detail_broad * sand_detail;
      }
    }
    color = fragment_color * T0 * 2;
    float baked = clamp(dot(fragment_color.rgb * 2.0, vec3(0.3, 0.59, 0.11)) * 0.5, 1.0 / 255.0, 0.6);
    enh_albedo_out = vec4(T0.rgb, baked);  // (a < 1: alpha 1 is tfrag3.frag's PBR-coded albedo)
    // (the detail material's occlusion, and its roughness coded as tfrag3.frag codes it)
    color.rgb *= mix(1.0, g_det.w, clamp(enh_detail_k.w * g_det_k, 0.0, 1.0));
    if (enh_detail == 2 && tune_detail_hfrag_pbr > 0.5) {
      float r = clamp(DET_SAND_R + (g_det.z - 0.5) * 2.0 * enh_detail_k.z * g_det_k, 0.03, 1.0);
      int code = 1 + int(r * 62.0 + 0.5);
      vec3 a8 = floor(clamp(T0.rgb, 0.0, 1.0) * 255.0 + 0.5);
      vec3 bits = vec3(float((code >> 4) & 3), float((code >> 2) & 3), float(code & 3));
      enh_albedo_out = vec4((a8 * 4.0 + bits) / 1023.0, baked * 3.0 >= 0.5 ? 1.0 : 0.0);
    }
  } else {
    color = fragment_color;
  }
  enh_normal_out = enh_pack_normal(n);

  if (tune_detail_hfrag_pbr <= 0.5 && enh_detail_sun.w > 0.0 && g_n_det != vec3(0.0)) {
    // (the light at most DET_SHADE_UP high: a sun straight above lights every small slope alike and
    // the grain vanished at noon; the sky's side light still shows it there)
    vec3 l = enh_detail_sun.xyz;
    if (l.y > DET_SHADE_UP) {
      vec2 d = dot(l.xz, l.xz) > 1e-6 ? normalize(l.xz) : vec2(0.6, 0.8);
      l = vec3(d.x * DET_SHADE_SIDE, DET_SHADE_UP, d.y * DET_SHADE_SIDE);
    }
    // (the bumped normal brought back onto the base's tangent plane: linear in the slopes, so the
    // grain lights and darkens about the same mean instead of darkening the sand as it grows)
    float shade = dot(g_n_det / max(dot(g_n_det, g_n_base), 0.3) - g_n_base, l);
    color.rgb *= 1.0 + clamp(shade, -0.5, 0.5) * tune_detail_shade * enh_detail_sun.w;
  }
   color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
