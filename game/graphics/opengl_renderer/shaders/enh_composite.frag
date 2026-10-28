#version 430 core

// Jak Enhanced Trilogy: composite of the 3D scene, drawn back over it before the HUD buckets.
// In order: heat haze, camera motion blur or FXAA, light depth of field, contrast-adaptive
// sharpen, ambient occlusion and sun shadows, distance/height fog, bloom and god rays, highlight
// roll-off, grading and vignette.

uniform sampler2D tex_T0;  // scene
uniform sampler2D tex_T1;  // bloom
uniform sampler2D tex_T2;  // ambient occlusion and sun shadows
uniform sampler2D tex_T3;  // depth (black when no 3D scene this frame)
uniform sampler2D tex_T4;  // god rays
uniform sampler2D tex_T7;  // indirect light (SSGI)
uniform sampler2D tex_T8;  // volumetric mist, half resolution (rgb scattered, a transmittance)
uniform int volume;
uniform sampler2DShadow tex_T9;  // sun shadow map
uniform float shadow_strength;   // 0 = no shadow map this frame
uniform mat4 light_mvp;          // world (game units) -> shadow map
uniform mat3 view_to_world;
uniform int light_count;        // dynamic lights (the game's glows), view space
uniform vec4 light_pos[32];     // xyz, radius (game units)
uniform vec4 light_color[32];
uniform float light_shadow;     // how dark what blocks a dynamic light makes it (0 = no shadows)
uniform int wake_count;         // Jak's recent path: world xyz (game units), age (s)
uniform vec4 wake[16];
uniform float wake_strength;
uniform int print_count;        // footprints and ruts: world xyz (game units), heading (rad)
uniform vec4 prints[48];
uniform vec2 print_info[48];    // age (s), kind (0/1 left/right foot, 2 ruts)
uniform int splash_count;       // Jak's last steps and landings: world xyz (game units),
uniform vec4 splash[16];         // w: age (s) + 10 * strength (1 step, 2 landing)
uniform float prints_strength;
uniform int puff_count;         // dust behind a vehicle or the board, spray off the water:
uniform vec4 puff[32];          // world xyz (game units), age (s)
uniform vec4 puff_info[32];     // size (m), kind (0 dust, 1 spray, 2 snow, 3 smoke), seed, life (s)
uniform float relief;           // relief from the textures' shading under the sun (0 = off; 2 = left half)
uniform float materials;        // metal and glossy surfaces: reflections and highlights (0 = off,
                                // < 0: show them, metal red, glossy blue; 2: left half only)
uniform int blast_count;        // explosions: view space + radius (game units), age (s)
uniform vec4 blast_pos[4];
uniform float blast_age[4];
uniform float blast_strength;
uniform int fire_count;         // the nearest fires: view space, flame size (game units)
uniform vec4 fire_pos[4];
uniform float fire_seed[4];
uniform vec3 fire;              // hot air strength, embers strength, time (s)
uniform int mote_count;         // fireflies or seeds drifting around Jak: view space (game units),
uniform vec4 mote[24];          // w: seed
uniform vec3 mote_style;        // kind (0 fireflies, 1 seeds and pollen), strength, time (s)
uniform vec2 vol_texel;
uniform float gi_strength;
uniform int ao_debug;
uniform sampler2D tex_T11;  // the level geometry's world normals (a = 0: none, use depth)

// ray-traced reflections: the loaded levels' bounding volume hierarchies (EnhancedRayTrace.h),
// world space in game units. Nodes: 2 vec4 (min, w: right child or first triangle; max, w: 0 or
// the leaf's triangle count). Triangles: 5 vec4 (v0, w: texture mean color; edge 1, w: time-of-day
// color; edge 2, w: texture layer or ~0; uv0, uv1; uv2). Palettes: 8 RGBA8 per time-of-day color.
layout(std430, binding = 0) readonly buffer RtNodes { vec4 rt_nodes[]; };
layout(std430, binding = 1) readonly buffer RtTris { vec4 rt_tris[]; };
layout(std430, binding = 2) readonly buffer RtPalettes { uint rt_pal[]; };
uniform int rt_mode;          // 0 off, 1 where the screen-space reflection misses, 2 traced only,
                              // 3 debug: the traced color alone on the water (magenta: missed)
uniform int rt_levels;
uniform ivec4 rt_base[16];    // per level: node, triangle, palette, texture layer base (-1: none)
uniform sampler2DArray tex_T12;  // the levels' textures, small
uniform vec4 rt_tod[8];       // the time-of-day palettes' weights (per channel)
uniform int normal_debug;
uniform vec4 proj;  // a, b, fx, fy (see enh_ssao.frag)
uniform vec2 texel;

uniform int fxaa;
uniform vec3 dof;  // start, end (game units), max radius (pixels)
uniform float sharpen;
uniform float bloom_strength;
uniform float shafts_strength;
uniform float saturation;
uniform float contrast;
uniform float warmth;
uniform float sun_haze;         // the area's warm haze lit by the sun over the distance (0..1)
uniform float golden;           // the late afternoon's amber look (0..1, the area's times the hour)
uniform float noon;             // the hours around noon (0..1)
uniform float haze_shadow;      // 1: the god rays texture's alpha has the haze's shadow
uniform float rock_detail;      // the area's rock picture over rock and cliffs (0 = off)
uniform float rock_detail_avg;  // that picture's average brightness
uniform sampler2D tex_T10;      // the rock picture (tiling, mipmapped)
uniform float sand;             // the area's olive and dull greens pushed toward golden sand (0..1)
uniform float vignette;

uniform vec3 up_view;   // world up in view space
uniform vec4 fog;       // density (1/game unit), height falloff (1/game unit), max amount, -
uniform vec3 fog_color;
uniform vec4 haze;      // strength (uv), start, end (game units), time (s)
uniform mat3 to_prev;   // view -> previous frame's view
uniform vec3 to_prev_offset;
uniform vec4 motion;    // previous fx, fy, strength (0 = off), max length (uv)
uniform vec2 motion_keep;  // sharp until, full blur from (game units)

uniform sampler2D tex_T5;  // depth with the near ocean's surface (black when none)
uniform sampler2D tex_T6;  // depth right after the mid/far ocean (black when none)
uniform sampler2D tex_T14; // depth right before the near ocean (black when none)
uniform float water_strength;
uniform vec3 right_view;  // world axes in view space
uniform vec3 fwd_view;
uniform vec3 cam_pos;     // world
uniform vec4 sun_dir;     // toward the sun in view space, w: glint strength (0 = none)
uniform vec3 water_deep_color;
uniform vec3 water_shallow_color;
uniform float water_unify;
uniform float water_swell_strength;  // open-sea swell (0 = ripples only)
uniform float water_refraction;      // how far shallow water bends what's under it (uv per slope)
uniform float water_caustics;        // caustics on the shallow sea bed
uniform int pond;                    // the water here is a murky pond (Jak 3's desert oases)
uniform float relight;      // sun relight strength (0 = off)  // how much of our water color replaces the game's

uniform vec4 color_mult;
uniform vec4 color_add;
uniform vec3 grade_lift;        // the area's grade: added in the shadows
uniform vec3 grade_gain;        // and multiplying the highlights
uniform sampler2D tex_T13;      // the sun shadow map as plain depths (soft shadows' blocker search)
uniform vec4 shadow_params;     // depth span (m), map side (m), penumbra per meter, soft (1) or not
uniform vec4 sky_sun;           // toward the sun (or moon) in view space, day or night; w: its height
uniform vec4 clouds;            // amount (0 = off), sky cover, time (s), night (0..1)
uniform int cloud_pass;         // 1: this draw only marches the clouds, a pixel for every 2x2
uniform sampler2D tex_T15;      // the clouds at a quarter of the pixels (last frame's in the pass)
uniform vec4 cloud_info;        // the clouds texture is ready, history to blend in, how much of it to keep
uniform float light_spill;      // at night the bright pixels light the surfaces near them
uniform float lens_flare;       // the sun in the lens (0 = off)
uniform float cloud_shadows;    // the clouds shade the ground (0 = off)
uniform vec2 night_dark;        // dark nights: how dark (0..1), the lights' gain
uniform vec4 player_view;       // Jak (view space, game units), w: valid
uniform int spot_count;         // vehicles' headlights: view space + strength, the way they point
uniform vec4 spot_pos[8];
uniform vec4 spot_dir[8];
uniform vec4 lantern;           // Jak's lantern in the desert at night: view space, strength
uniform vec4 town_glow;         // a town lighting the night sky over it: world x, z, strength
uniform int beacon_count;       // far glows drawn as points of light: view space + size
uniform vec4 beacon_pos[24];
uniform vec4 beacon_color[24];  // rgb, strength
uniform float city_glow;        // a lit city at night (0..1): its glow on the low sky and the haze
uniform vec4 weather;           // rain, dust (0..1), time (s), wet ground (0..1)
uniform float puddle_level;     // the puddles' water (0..1): outlasts the wet ground, shrinks to the dips
uniform vec4 weather2;          // thunderstorm, ground fog, the sun out after the rain, lightning flash
uniform vec4 dust_wind;         // the wind (world xz, unit), how far it carried the dust (m)
uniform float sheet;            // sheet lightning: the whole sky lit from inside the clouds (0..1)
uniform vec2 snow_heat;          // snowfall, hot air over lava (0..1)
uniform float snow_cover;        // snow lying on the ground (0..1)
uniform vec4 strike;            // a bolt to the ground: its foot's elevation (sine), the cloud base's, its distance (game units), on
uniform vec4 sky_kind;          // the evening's sky: stars shown (0..1), the moon's lit part, red eclipse, red sunset now
uniform float sunny;            // a bright summer day (Jak 1's Sandover): deep blue sky, white clouds
uniform vec4 bolt;              // lightning bolt: azimuth (rad), seed, age (s), strength
uniform float close_up;          // cutscenes: nearer than this (game units) no sun shadow map (0 = off)
uniform vec3 cine_dof;          // cutscenes: focus (view z, game units), aperture, max blur (px; 0 off)
uniform float blast_sparks;     // explosions' sparks and hot air
uniform float blast_seed[4];
uniform vec4 blast_dir[4];      // gun blasts: where it's fired (view space), w: kind (0 explosion,
                                // 1 red gun, 2 red gun charging: only sparks on the gun, 3 a
                                // charged shot: wide, red going fuchsia at its edges)

in vec2 tex_coord;
out vec4 color;

float luma(vec3 c) {
  return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// gradient noise, for water that doesn't repeat
// (without sine: plain arithmetic, cheaper, and gnoise calls it four times; Dave Hoskins' hash)
vec2 hash2(vec2 q) {
  vec3 p3 = fract(q.xyx * vec3(0.1031, 0.1030, 0.0973));
  p3 += dot(p3, p3.yzx + 33.33);
  return -1.0 + 2.0 * fract((p3.xx + p3.yz) * p3.zy);
}

float gnoise(vec2 x) {
  vec2 i = floor(x);
  vec2 f = fract(x);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(dot(hash2(i), f), dot(hash2(i + vec2(1, 0)), f - vec2(1, 0)), u.x),
             mix(dot(hash2(i + vec2(0, 1)), f - vec2(0, 1)), dot(hash2(i + vec2(1, 1)), f - vec2(1, 1)), u.x),
             u.y);
}

// water height (m) at a world point (m): four octaves moving in different directions, each
// faded out when its waves get smaller than a pixel (foot: meters per pixel) so nothing aliases
float water_height(vec2 w, float t, float foot) {
  const float freq[4] = float[](0.12, 0.35, 0.95, 2.6);
  const float amp[4] = float[](0.045, 0.016, 0.0055, 0.0018);
  const vec2 drift[4] = vec2[](vec2(0.35, 0.2), vec2(-0.25, 0.4), vec2(0.5, -0.3), vec2(-0.6, -0.45));
  float h = 0.0;
  mat2 turn = mat2(0.8, -0.6, 0.6, 0.8);
  vec2 q = w;
  for (int i = 0; i < 4; i++) {
    float fade = 1.0 - smoothstep(0.25, 0.7, foot * freq[i]);
    h += amp[i] * fade * gnoise(q * freq[i] + drift[i] * t * freq[i] * 2.0 + float(i) * 17.3);
    q = turn * q;
  }
  return h;
}

// open-sea swell (m): long waves crossing at an angle, only in deep water (calm in the shallows)
float water_swell(vec2 w, float t) {
  return 0.22 * sin(dot(w, vec2(0.13, 0.07)) + t * 0.8) +
         0.12 * sin(dot(w, vec2(-0.09, 0.17)) + t * 1.1) +
         0.06 * sin(dot(w, vec2(0.27, -0.21)) + t * 1.7);
}

// caustics: the bright net that waves focus on a shallow sea bed (0..1), two drifting layers
float caustics(vec2 w, float t) {
  float a = 1.0 - abs(gnoise(w * 1.3 + vec2(t * 0.30, t * 0.17)));
  float b = 1.0 - abs(gnoise(w * 1.7 * mat2(0.8, -0.6, 0.6, 0.8) - vec2(t * 0.21, -t * 0.26)));
  return pow(a, 6.0) * pow(b, 6.0) * 4.0 + pow(min(a, b), 10.0);
}

float view_z(float d) {
  return d > 1e-7 ? abs(proj.y / (d * 2.0 - 1.0 - proj.x)) : 0.0;
}

// screen-space shadow toward a dynamic light: a few steps through the depth buffer, up to 3 m
// (and short of the light itself, which often sits inside its lamp)
// (reach: how far toward the light the ray looks, game units; steps: how many samples)
float light_blocked_by(vec3 from, vec3 lp, float jitter, float reach, int steps) {
  vec3 dir = lp - from;
  float len = length(dir);
  vec3 stp = dir / len * (reach / float(steps));
  for (int s = 0; s < steps; s++) {
    vec3 q = from + stp * (float(s) + jitter);
    if (q.z < 0.3 * 4096.0) {
      break;
    }
    vec2 quv = vec2(q.x * proj.z / q.z, q.y * proj.w / q.z) * 0.5 + 0.5;
    if (quv.x < 0.0 || quv.y < 0.0 || quv.x > 1.0 || quv.y > 1.0) {
      break;
    }
    float sd = texture(tex_T3, quv).r;
    if (sd > 1e-7) {
      float sz = view_z(sd);
      // something in front of the ray (but not a whole building's depth: then it's beside it)
      float gap = q.z - sz;
      if (gap > 0.04 * 4096.0 + q.z * 0.004 && gap < 1.5 * 4096.0) {
        return 1.0;
      }
    }
  }
  return 0.0;
}

float light_blocked(vec3 from, vec3 lp, float jitter) {
  return light_blocked_by(from, lp, jitter, min(length(lp - from) * 0.85, 3.0 * 4096.0), 8);
}

vec3 view_pos(vec2 uv, float d) {
  float z = proj.y / (d * 2.0 - 1.0 - proj.x);
  vec2 ndc = uv * 2.0 - 1.0;
  return vec3(ndc.x * z / proj.z, ndc.y * z / proj.w, z);
}

// how much of the sun the shadow map says is blocked at a world point (game units), 0..1
// the surface normal in view space, toward the camera: the level's own normal where it wrote one,
// else rebuilt from the depth buffer (the neighbor on each axis with the smaller depth jump)
vec3 surface_normal(vec2 uv, vec3 p) {
  vec3 nn;
  vec4 g = texture(tex_T11, uv);
  if (g.a > 0.17) {
    nn = normalize((g.rgb * 2.0 - 1.0) * view_to_world);  // world -> view (transpose)
  } else {
    vec3 pr = view_pos(uv + vec2(texel.x, 0), texture(tex_T3, uv + vec2(texel.x, 0)).r);
    vec3 pl = view_pos(uv - vec2(texel.x, 0), texture(tex_T3, uv - vec2(texel.x, 0)).r);
    vec3 pu = view_pos(uv + vec2(0, texel.y), texture(tex_T3, uv + vec2(0, texel.y)).r);
    vec3 pd = view_pos(uv - vec2(0, texel.y), texture(tex_T3, uv - vec2(0, texel.y)).r);
    vec3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
    vec3 dy = abs(pu.z - p.z) < abs(p.z - pd.z) ? pu - p : p - pd;
    nn = normalize(cross(dx, dy));
  }
  return dot(nn, -p) < 0.0 ? -nn : nn;
}

// the nearest level triangle along o + d * t (world, game units, d unit length), up to tmax:
// its color in the game's own baked light at this hour, and how far it is
bool rt_trace(vec3 o, vec3 d, float tmax, out vec3 color, out float dist) {
  vec3 dd = vec3(abs(d.x) < 1e-7 ? 1e-7 : d.x, abs(d.y) < 1e-7 ? 1e-7 : d.y,
                 abs(d.z) < 1e-7 ? 1e-7 : d.z);
  vec3 inv = 1.0 / dd;
  float best = tmax;
  int best_tri = -1;
  int best_level = 0;
  vec2 best_uv = vec2(0.0);
  int stack[32];
  for (int l = 0; l < rt_levels; l++) {
    int nb = rt_base[l].x;
    int tb = rt_base[l].y;
    int sp = 0;
    stack[sp++] = 0;
    int steps = 0;
    while (sp > 0 && steps < 400) {
      steps++;
      int ni = stack[--sp];
      vec4 a = rt_nodes[(nb + ni) * 2];
      vec4 b = rt_nodes[(nb + ni) * 2 + 1];
      vec3 t0 = (a.xyz - o) * inv;
      vec3 t1 = (b.xyz - o) * inv;
      vec3 tlo = min(t0, t1);
      vec3 thi = max(t0, t1);
      float tn = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
      float tf = min(min(thi.x, thi.y), min(thi.z, best));
      if (tn > tf) {
        continue;
      }
      int first = int(floatBitsToUint(a.w));
      int count = int(floatBitsToUint(b.w));
      if (count > 0) {
        for (int k = 0; k < count; k++) {
          int ti = (tb + first + k) * 5;
          vec3 v0 = rt_tris[ti].xyz;
          vec3 e1 = rt_tris[ti + 1].xyz;
          vec3 e2 = rt_tris[ti + 2].xyz;
          vec3 pv = cross(d, e2);
          float det = dot(e1, pv);
          if (abs(det) < 1e-4) {
            continue;
          }
          float id = 1.0 / det;
          vec3 tv = o - v0;
          float u = dot(tv, pv) * id;
          if (u < 0.0 || u > 1.0) {
            continue;
          }
          vec3 qv = cross(tv, e1);
          float v = dot(d, qv) * id;
          if (v < 0.0 || u + v > 1.0) {
            continue;
          }
          float t = dot(e2, qv) * id;
          if (t > 0.0 && t < best) {
            best = t;
            best_tri = ti;
            best_level = l;
            best_uv = vec2(u, v);
          }
        }
      } else if (sp < 31) {
        stack[sp++] = first;   // the right child (the farther one, usually: later)
        stack[sp++] = ni + 1;  // the left child, next
      }
    }
  }
  if (best_tri < 0) {
    return false;
  }
  vec3 tex = unpackUnorm4x8(floatBitsToUint(rt_tris[best_tri].w)).rgb;
  uint layer = floatBitsToUint(rt_tris[best_tri + 2].w);
  if (layer != 0xffffffffu && rt_base[best_level].w >= 0) {
    vec4 a = rt_tris[best_tri + 3];
    vec2 uv2 = rt_tris[best_tri + 4].xy;
    vec2 tuv = a.xy * (1.0 - best_uv.x - best_uv.y) + a.zw * best_uv.x + uv2 * best_uv.y;
    // coarser further away (the ray's footprint grows with the distance)
    float lod = clamp(log2(best / (6.0 * 4096.0)), 0.0, 6.0);
    tex = textureLod(tex_T12, vec3(tuv, float(rt_base[best_level].w + int(layer))), lod).rgb;
  }
  int pi = (rt_base[best_level].z + int(floatBitsToUint(rt_tris[best_tri + 1].w))) * 8;
  vec4 tod = vec4(0.0);
  for (int k = 0; k < 8; k++) {
    tod += unpackUnorm4x8(rt_pal[pi + k]) * rt_tod[k];
  }
  // as the game's background shader: (time-of-day color * 2) * texture
  color = min(tod.rgb, vec3(1.0)) * 2.0 * tex;
  // the distance haze the game would put on it
  color = mix(color, fog_color, smoothstep(40.0 * 4096.0, 450.0 * 4096.0, best) * 0.85);
  dist = best;
  return true;
}

// dark nights on a color taken from the scene (reflections read it before the night darkens it):
// the same darkening, what shines by itself kept
vec3 night_dim(vec3 col) {
  if (night_dark.x <= 0.0) {
    return col;
  }
  float l = luma(col);
  vec3 moon = vec3(l) * mix(vec3(0.7, 0.82, 1.1), vec3(1.1, 0.55, 0.45), sky_kind.z);
  vec3 darkened = mix(col, moon, 0.45 * night_dark.x) * (1.0 - 0.9 * night_dark.x);
  return mix(darkened, col, smoothstep(0.4, 0.85, l));
}

// a reflection: the screen-space one where it found something (ssr_hit), else what the ray meets
// in the levels (rt_mode), else env (the screen where the ray vanishes, or the fog color)
vec3 reflection_with_rt(vec3 p, vec3 r, vec3 env, vec3 ssr, float ssr_hit, float tmax,
                        float amount) {
  if (rt_mode >= 2) {
    ssr_hit = 0.0;
  }
  if (rt_mode == 3) {
    env = vec3(1.0, 0.0, 1.0);
  }
  if (rt_mode > 0 && ssr_hit < 0.99) {
    vec3 o = view_to_world * p + cam_pos;
    vec3 d = normalize(view_to_world * r);
    vec3 rc;
    float rd;
    // start a little off the surface, so it doesn't find itself
    if (amount > 0.0 && rt_trace(o + d * (0.1 * 4096.0), d, tmax, rc, rd)) {
      env = mix(env, rc, amount);
    }
  }
  return night_dim(mix(env, ssr, ssr_hit));
}

// what a surface reflects along r (view space): the scene where the ray meets the depth buffer
// (a short march), else the screen where the ray vanishes (the sky, far hills); hit: how sure
vec3 trace_reflection(vec3 p, vec3 r, float z, out float hit) {
  hit = 0.0;
  vec3 env = fog_color;
  if (r.z > 1e-3) {
    vec2 env_uv = clamp(0.5 * vec2(proj.z * r.x, proj.w * r.y) / r.z + 0.5, vec2(0.002), vec2(0.998));
    env = texture(tex_T0, env_uv).rgb;
  }
  vec3 ssr = env;
  float step_len = 0.15 * 4096.0 + z * 0.01;
  float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) +
                    haze.w * 7.13);
  vec3 q = p + r * (0.05 * 4096.0 + z * 0.002 + step_len * jit);
  for (int i = 0; i < 20; i++) {
    float used = step_len;
    q += r * step_len;
    step_len *= 1.15;
    if (q.z <= 0.0) {
      break;
    }
    vec2 quv = 0.5 * vec2(proj.z * q.x, proj.w * q.y) / q.z + 0.5;
    if (quv.x < 0.0 || quv.y < 0.0 || quv.x > 1.0 || quv.y > 1.0) {
      break;
    }
    float sd = texture(tex_T3, quv).r;
    if (sd <= 1e-7) {
      continue;
    }
    float scene_z = view_z(sd);
    if (q.z > scene_z && q.z - scene_z < step_len + 2048.0) {
      // halve back and forth between the last step in front of the scene and this one
      vec3 lo = q - r * used;
      vec3 hi = q;
      for (int k = 0; k < 5; k++) {
        vec3 mid = 0.5 * (lo + hi);
        vec2 muv = 0.5 * vec2(proj.z * mid.x, proj.w * mid.y) / max(mid.z, 1.0) + 0.5;
        float md = texture(tex_T3, muv).r;
        if (md > 1e-7 && mid.z > view_z(md)) {
          hi = mid;
        } else {
          lo = mid;
        }
      }
      if (hi.z > 0.0) {
        quv = 0.5 * vec2(proj.z * hi.x, proj.w * hi.y) / hi.z + 0.5;
      }
      vec2 edge = min(quv, 1.0 - quv);
      hit = smoothstep(0.0, 0.06, min(edge.x, edge.y));
      ssr = texture(tex_T0, quv).rgb;
      break;
    }
  }
  vec3 refl = reflection_with_rt(p, r, env, ssr, hit, 60.0 * 4096.0, 1.0);
  if (rt_mode > 0) {
    hit = 1.0;  // traced or not, it's the best there is
  }
  return refl;
}

// a per-pixel angle that changes every frame (interleaved gradient noise; the TAA averages it)
float pixel_angle() {
  return 6.2832 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) +
                        haze.w * 7.13);
}

// the i-th of n points spread evenly over the unit disk (Vogel), turned by rot
vec2 vogel(int i, int n, float rot) {
  float r = sqrt((float(i) + 0.5) / float(n));
  float a = float(i) * 2.39996 + rot;
  return r * vec2(cos(a), sin(a));
}

float sun_shadow(vec3 w) {
  vec4 l = light_mvp * vec4(w, 1.0);
  vec3 s = l.xyz * 0.5 + 0.5;
  if (any(lessThan(s, vec3(0.0))) || any(greaterThan(s, vec3(1.0)))) {
    return 0.0;
  }
  // fade out toward the edge of the map
  vec2 e = abs(s.xy * 2.0 - 1.0);
  float fade = 1.0 - smoothstep(0.8, 1.0, max(e.x, e.y));
  if (shadow_params.w > 0.0) {
    // soft shadows (PCSS): how far above this point the things in the way are (the blockers'
    // average depth, 12 taps over 1.5 m) sets how wide the penumbra is: sharp where a wall meets
    // the ground, soft under a high roof
    float rot = pixel_angle();
    float search = 1.5 / shadow_params.y;
    float blockers = 0.0;
    float found = 0.0;
    for (int i = 0; i < 12; i++) {
      float bd = texture(tex_T13, s.xy + vogel(i, 12, rot) * search).r;
      if (bd < s.z - 0.00012) {
        blockers += bd;
        found += 1.0;
      }
    }
    if (found == 0.0) {
      return 0.0;
    }
    float gap_m = (s.z - blockers / found) * shadow_params.x;
    float penumbra_m = clamp(gap_m * shadow_params.z, 0.05, 1.2);
    float r = penumbra_m / shadow_params.y;
    // a wider filter reaches further over sloped ground: more bias with it
    float bias = 0.00012 + penumbra_m / shadow_params.x * 0.6;
    float lit = 0.0;
    for (int i = 0; i < 16; i++) {
      lit += texture(tex_T9, vec3(s.xy + vogel(i, 16, rot + 1.3) * r, s.z - bias));
    }
    return (1.0 - lit / 16.0) * fade;
  }
  vec2 t = vec2(1.0 / 4096.0);
  float lit = 0.0;
  for (int y = -1; y <= 2; y++) {
    for (int x = -1; x <= 2; x++) {
      lit += texture(tex_T9, vec3(s.xy + (vec2(x, y) - 0.5) * t, s.z - 0.00012));
    }
  }
  return (1.0 - lit / 16.0) * fade;
}

// a view direction (view space) in world axes (y up)
vec3 world_dir(vec3 dv) {
  return vec3(dot(right_view, dv), dot(up_view, dv), dot(fwd_view, dv));
}

float hash1(vec2 q) {
  return fract(sin(dot(q, vec2(127.1, 311.7))) * 43758.5453);
}

float fbm(vec2 q) {
  float sum = 0.0, amp = 0.5;
  mat2 turn = mat2(0.8, -0.6, 0.6, 0.8);
  for (int i = 0; i < 5; i++) {
    sum += amp * gnoise(q);
    q = turn * q * 2.03 + 3.7;
    amp *= 0.5;
  }
  return sum;
}

// 3D value noise on an integer hash (no sin: stays clean kilometers from the origin)
float ihash3(vec3 c) {
  uvec3 v = uvec3(ivec3(c) + ivec3(1 << 20));
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z;
  return float(v.x & 0xffffffu) / 16777215.0;
}

float vnoise3(vec3 x) {
  vec3 i = floor(x);
  vec3 f = fract(x);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(ihash3(i), ihash3(i + vec3(1, 0, 0)), f.x),
                 mix(ihash3(i + vec3(0, 1, 0)), ihash3(i + vec3(1, 1, 0)), f.x), f.y),
             mix(mix(ihash3(i + vec3(0, 0, 1)), ihash3(i + vec3(1, 0, 1)), f.x),
                 mix(ihash3(i + vec3(0, 1, 1)), ihash3(i + vec3(1, 1, 1)), f.x), f.y),
             f.z);
}

// the dust and spray puffs along the view ray, up to the scene (m): soft balls that grow, rise (the
// spray falls back) and fade, broken up by noise; dust drifts with the wind. Premultiplied color,
// coverage.
vec4 ride_puffs(vec3 ray_view, float scene_dist, vec3 sand, vec3 spray, vec3 sun_w) {
  vec3 o = cam_pos / 4096.0;
  vec3 d = view_to_world * ray_view;
  vec3 col = vec3(0.0);
  float trans = 1.0;
  for (int i = 0; i < puff_count; i++) {
    float age = puff[i].w;
    float u = age / puff_info[i].w;
    if (u >= 1.0) {
      continue;
    }
    int kind = int(puff_info[i].y + 0.5);
    bool dust = kind != 1;
    float seed = puff_info[i].z;
    vec3 c = puff[i].xyz / 4096.0;
    float r;
    if (kind == 3) {
      // smoke: a column that swells as it climbs and leans with the wind
      r = puff_info[i].x * (0.5 + 1.8 * sqrt(u));
      c.y += r * 0.3 + 1.4 * age;
      c.xz += dust_wind.xy * age * 2.5;
    } else if (kind == 2) {
      // snow kicked up: a quick white burst that settles back down
      r = puff_info[i].x * (0.5 + 1.2 * sqrt(u));
      c.y += r * 0.25 + 0.6 * age - 0.5 * age * age;
      c.xz += dust_wind.xy * age;
    } else if (dust) {
      r = puff_info[i].x * (0.45 + 1.6 * sqrt(u));
      // kicked up off the ground: low at first, its lower half in the sand, rising slowly
      c.y += r * 0.2 + 0.3 * age;
      c.xz += dust_wind.xy * age * 1.5;
    } else {
      r = puff_info[i].x * (0.5 + 1.1 * sqrt(u));
      c.y += r * 0.3 + 3.0 * age - 4.0 * age * age;
    }
    vec3 oc = c - o;
    float tc = dot(oc, d);
    float d2 = dot(oc, oc) - tc * tc;
    if (d2 >= r * r || tc + r < 0.0) {
      continue;
    }
    float half_chord = sqrt(r * r - d2);
    float t0 = max(tc - half_chord, 0.0);
    float t1 = min(tc + half_chord, scene_dist);
    if (t1 <= t0) {
      continue;
    }
    // four samples through it: billows of 3D noise, thinning toward the edge
    float dens = 0.0;
    vec3 lit_dir = vec3(0.0);
    float dt = (t1 - t0) / 4.0;
    for (int k = 0; k < 4; k++) {
      vec3 q = (o + d * (t0 + dt * (float(k) + 0.5)) - c) / r;
      float fall = max(1.0 - dot(q, q), 0.0);
      vec3 nq = q * 1.9 + vec3(seed * 17.0, -age * 0.9, seed * 5.0);
      float n = vnoise3(nq) * 0.6 + vnoise3(nq * 2.3 + 4.0) * 0.4;
      float sd = fall * smoothstep(0.42, 0.72, n + fall * 0.2);
      if (!dust) {
        // drops: a fine grain on top of the mist
        sd = sd * 0.6 + fall * smoothstep(0.6, 0.8, vnoise3(q * 9.0 + vec3(seed * 9.0, age * 6.0, 0.0))) * 0.8;
      }
      dens += sd * dt / r;
      lit_dir += q * sd;
    }
    float lit = 0.75 + 0.25 * dot(normalize(lit_dir + vec3(0.0, 1e-3, 0.0)), sun_w);
    float a;
    vec3 pc;
    if (kind == 3) {
      // (thin and grey: a wisp, not a ball; steamier in the rain)
      a = min(dens * (1.0 - smoothstep(0.15, 1.0, u)) * smoothstep(0.0, 0.15, age) * 0.75, 0.5);
      pc = vec3(luma(sand)) * mix(0.7, 0.95, weather.x) * lit;
    } else if (kind == 2) {
      a = dens * (1.0 - smoothstep(0.2, 1.0, u)) * smoothstep(0.0, 0.04, age) * 1.4;
      pc = vec3(luma(spray)) * vec3(1.05, 1.1, 1.18) * lit;
    } else if (dust) {
      a = dens * (1.0 - smoothstep(0.3, 1.0, u)) * smoothstep(0.0, 0.06, age) * 1.3;
      pc = sand * lit;
    } else {
      a = dens * (1.0 - smoothstep(0.2, 1.0, u)) * 1.2;
      pc = spray * lit;
    }
    a = clamp(a, 0.0, 0.8);
    col += trans * a * pc;
    trans *= 1.0 - a;
  }
  return vec4(col, 1.0 - trans);
}

// the sandstorm's air at a world point (m): rolling clouds of sand stretched along the wind and
// carried by it (the game storm's own wind), thicker near the ground, with fine streamers blowing
// low over it close by. Roughly 1 on average.
float dust_density(vec3 w, float near) {
  vec2 dir = dust_wind.xy;
  vec2 q = w.xz - dust_wind.zw;
  // along the wind, up, across it; the clouds boil slowly as they go
  vec3 s = vec3(dot(q, dir), w.y + clouds.z * 0.35, dot(q, vec2(-dir.y, dir.x)));
  float big = vnoise3(s * vec3(1.0 / 26.0, 1.0 / 9.0, 1.0 / 11.0));
  float mid = vnoise3(s * vec3(1.0 / 8.0, 1.0 / 3.5, 1.0 / 3.5) + vec3(17.0, 3.0, 5.0));
  float n = big * 0.65 + mid * 0.35;
  float dens = 0.2 + 1.6 * smoothstep(0.32, 0.8, n);
  // low streamers: the sand that skims the ground, fast, near the camera only
  if (near > 0.0) {
    vec3 sf = vec3(dot(q, dir) * 2.0, w.y, dot(q, vec2(-dir.y, dir.x)));
    float fine = vnoise3(sf * vec3(1.0 / 3.0, 1.0 / 0.5, 1.0 / 0.4) + vec3(3.0, 0.0, 9.0));
    dens += near * 0.9 * smoothstep(0.55, 0.9, fine);
  }
  // thicker toward the ground (taken as a little under the camera)
  float h = w.y - (cam_pos.y / 4096.0 - 3.0);
  return dens * mix(1.0, 0.35, smoothstep(0.0, 40.0, h));
}

// how much of the dust hides the scene along a view ray (view space, unit) up to dist (m), marched
// through the volume close up (where its shapes show) and in one step past that
float dust_cover(vec3 ray_view, float dist, float amount, float jitter) {
  vec3 wd = world_dir(ray_view);
  vec3 o = cam_pos / 4096.0;
  float k = 0.006 * amount + 0.03 * amount * amount * amount;  // extinction per meter
  float march = min(dist, 160.0);
  float tau = 0.0;
  float prev = 0.0;
  const int N = 12;
  for (int i = 0; i < N; i++) {
    // more steps near the camera
    float u = (float(i) + jitter) / float(N);
    float t = march * u * u;
    float near = 1.0 - smoothstep(4.0, 14.0, t);
    tau += dust_density(o + wd * t, near) * (t - prev) * k;
    prev = t;
  }
  tau += dust_density(o + wd * march, 0.0) * (march - prev) * k;
  // the rest of the way, at the average
  tau += max(dist - march, 0.0) * k * 0.8;
  return 1.0 - exp(-tau);
}

// low fog lying on the ground (thickest a couple of meters under the camera, thinning upward):
// the height fog integral along the ray to dist (m)
float ground_fog(vec3 ray_view, float dist, float amount) {
  vec3 wd = world_dir(ray_view);
  const float H = 7.0;
  float base = exp(-2.0 / H);  // the density at the camera, which is ~2 m over the ground
  float dy = wd.y;
  float path = abs(dy) < 1e-3 ? dist : H / dy * (1.0 - exp(-dy * dist / H));
  return 1.0 - exp(-0.035 * amount * base * max(path, 0.0));
}

// lightning, far out in the sky: a jagged channel from the clouds down to the horizon at the
// bolt's azimuth. The leader steps down from the top in under a tenth of a second (its tip
// brightest), then the return stroke lights the whole channel and its branches, it strikes again
// twice and fades. Returns the light (unitless, ~1 at the core).
float bolt_jag(float u, float seed) {
  // the channel's sideways offset (rad) at u (0 at the clouds, 1 at the horizon)
  return (gnoise(vec2(u * 5.0, seed)) * 0.07 + gnoise(vec2(u * 22.0, seed + 5.0)) * 0.022 +
          gnoise(vec2(u * 70.0, seed + 11.0)) * 0.006) * (0.4 + u);
}

float lightning_bolt(vec3 ray_view, float px) {
  // the cloud base where it comes out (sine of the elevation): lower for a farther bolt
  // (a strike: from the cloud base above it down to its foot on the ground; the nearer, the bigger
  // it all is on screen: s scales the channel's sideways angles)
  bool ground = strike.w > 0.0;
  float top = ground ? strike.y : 0.14 + 0.16 * fract(bolt.y * 0.37);
  float bottom = ground ? strike.x : 0.0;
  float s = ground ? clamp(200.0 * 4096.0 / strike.z, 0.5, 10.0) : 1.0;
  float a = bolt.z;
  if (bolt.w <= 0.0 || a > 0.42) {
    return 0.0;
  }
  vec3 wd = world_dir(ray_view);
  float el = wd.y;
  if (el < bottom - 0.01 || el > top) {
    return 0.0;
  }
  float da = (mod(atan(wd.x, wd.z) - bolt.x + 3.14159, 6.28318) - 3.14159) / s;
  if (abs(da) > 0.5) {
    return 0.0;
  }
  float u = (top - el) / max(top - bottom, 1e-4);
  // the leader's tip
  float lead = clamp(a / 0.09, 0.0, 1.0);
  if (u > lead) {
    return 0.0;
  }
  float w = max(px * 1.3 / s, 0.0012);
  float x = da - bolt_jag(u, bolt.y);
  float core = exp(-x * x / (w * w));
  float glow = exp(-x * x / 0.0012) * 0.3 + exp(-x * x / 0.015) * 0.08;
  // branches off the channel, reaching sideways and down
  for (int k = 0; k < 3; k++) {
    float b0 = 0.18 + 0.22 * float(k) + 0.08 * fract(bolt.y * (1.3 + float(k)));
    float len = 0.2 + 0.1 * fract(bolt.y * 3.7 + float(k));
    float bu = (u - b0) / len;
    if (bu > 0.0 && bu < 1.0) {
      float side = fract(bolt.y * 7.1 + float(k) * 0.37) > 0.5 ? 1.0 : -1.0;
      float bx = da - bolt_jag(b0, bolt.y) - side * bu * len * 0.45 -
                 gnoise(vec2(u * 40.0, bolt.y + 20.0 + float(k))) * 0.012;
      float fade = 1.0 - bu;
      core += exp(-bx * bx / (w * w * 0.6)) * 0.6 * fade;
      glow += exp(-bx * bx / 0.0006) * 0.12 * fade;
    }
  }
  // over time: the dim leader with a bright tip, the return stroke, two restrikes, fading
  float bright;
  if (a < 0.09) {
    bright = 0.3 + 2.0 * exp(-pow((u - lead) / 0.03, 2.0));
  } else {
    float t = a - 0.09;
    bright = 1.6 * exp(-t * 25.0) + 1.1 * exp(-pow((t - 0.08) * 60.0, 2.0)) +
             0.7 * exp(-pow((t - 0.18) * 60.0, 2.0));
    bright *= 1.0 - smoothstep(0.25, 0.33, t);
  }
  // out of the clouds at the top
  float in_cloud = smoothstep(0.0, 0.06, u);
  return (core + glow) * bright * in_cloud * bolt.w;
}

// the rainbow opposite the sun after the rain: the primary bow at 42 degrees, red outside, and a
// faint secondary one at 51 with its colors the other way round
vec3 rainbow(vec3 ray_view) {
  float a = acos(clamp(dot(ray_view, -sky_sun.xyz), -1.0, 1.0)) * 57.2958;
  vec3 col = vec3(0.0);
  for (int i = 0; i < 2; i++) {
    float center = i == 0 ? 41.5 : 51.0;
    float width = i == 0 ? 3.2 : 5.0;
    float t = (a - center) / width + 0.5;  // 0 violet .. 1 red (the other way for the secondary)
    if (t > 0.0 && t < 1.0) {
      float tt = i == 0 ? t : 1.0 - t;
      float hue = (1.0 - tt) * 0.78;
      vec3 rgb = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
      col += rgb * smoothstep(0.0, 0.25, t) * smoothstep(1.0, 0.75, t) * (i == 0 ? 1.0 : 0.35);
    }
  }
  return col;
}

// the night sky over the game's: stars in two layers (cells on a cube of directions; twinkling,
// bluish to warm), a faint milky way, the moon's disk and halo; hidden where the game's sky is
// bright (its clouds), by the weather and by a city's glow
vec3 night_sky(vec3 dv, vec3 base, float px) {
  vec3 wd = world_dir(dv);
  float up = wd.y;
  vec3 add = vec3(0.0);
  float clear = (1.0 - smoothstep(0.1, 0.28, luma(base))) *
                (1.0 - clamp((clouds.y - 0.45) * 1.8, 0.0, 1.0)) * (1.0 - min(weather.x, 1.0)) *
                (1.0 - min(weather.y * 1.5, 1.0)) * (1.0 - min(weather2.y, 1.0)) * smoothstep(0.0, 0.15, up);
  if (clear > 0.0) {
    vec3 a = abs(wd);
    vec2 f;
    float face;
    if (a.x > a.y && a.x > a.z) {
      f = wd.yz / a.x;
      face = wd.x > 0.0 ? 0.0 : 1.0;
    } else if (a.y > a.z) {
      f = wd.xz / a.y;
      face = wd.y > 0.0 ? 2.0 : 3.0;
    } else {
      f = wd.xy / a.z;
      face = wd.z > 0.0 ? 4.0 : 5.0;
    }
    // the milky way: a band across the sky, mottled
    vec3 band_n = normalize(vec3(0.35, 0.45, 0.82));
    float bd = dot(wd, band_n);
    float band = exp(-bd * bd / 0.035) * (0.4 + 0.9 * vnoise3(wd * 7.0) * vnoise3(wd * 19.0 + 3.0));
    float city_dim = 1.0 - 0.75 * city_glow;
    add += vec3(0.55, 0.6, 0.78) * band * 0.045 * city_dim * sky_kind.x * sky_kind.x;
    for (int layer = 0; layer < 2; layer++) {
      float cells = layer == 0 ? 70.0 : 220.0;
      vec2 g = (f * 0.5 + 0.5) * cells;
      vec2 cid = floor(g);
      vec3 key = vec3(cid, face + 7.0 * float(layer));
      float h = ihash3(key);
      // (more of the faint ones along the milky way)
      float thr = layer == 0 ? 0.9 : 0.78 - 0.25 * band;
      // (a hazy or moonlit night shows only some of them: the brightest stay)
      if (h > thr && h > mix(1.0, thr, sky_kind.x)) {
        vec2 sp = vec2(ihash3(key + 11.0), ihash3(key + 23.0)) * 0.7 + 0.15;
        float ang = length(g - cid - sp) / cells * 1.5708;
        float size = px * (layer == 0 ? 1.1 : 0.75);
        float shape = exp(-ang * ang / (size * size));
        float b = (h - thr) / (1.0 - thr);
        b = layer == 0 ? 0.25 + 2.2 * b * b * b : 0.15 + 0.5 * b;
        float tw = 0.7 + 0.3 * sin(clouds.z * (2.0 + 5.0 * ihash3(key + 5.0)) + h * 60.0);
        vec3 tint = mix(vec3(0.72, 0.82, 1.0), vec3(1.0, 0.86, 0.66), ihash3(key + 31.0));
        add += tint * shape * b * tw * (layer == 0 ? 1.0 : city_dim);
      }
    }
    add *= clear;
  }
  // the moon (the night's light comes from it): a pale disk, darker seas, a soft halo
  float md = dot(dv, sky_sun.xyz);
  if (md > 0.9 && sky_sun.w > 0.0) {
    float mclear = (1.0 - clamp((clouds.y - 0.6) * 2.0, 0.0, 1.0)) * (1.0 - min(weather.x + weather.y, 1.0));
    float rr = 0.054;  // (4x the real moon: a big storybook moon)
    float off = sqrt(max(2.0 * (1.0 - md), 0.0));
    float disk = 1.0 - smoothstep(rr - px, rr + px, off);
    vec3 side = normalize(cross(sky_sun.xyz, vec3(0.0, 1.0, 0.0)) + 1e-4);
    vec3 upv = cross(side, sky_sun.xyz);
    vec2 mq = vec2(dot(dv, side), dot(dv, upv)) / rr;
    float seas = smoothstep(0.45, 0.7, vnoise3(vec3(mq * 2.2, 1.7)) * 0.7 + vnoise3(vec3(mq * 5.0, 4.1)) * 0.3);
    float limb = sqrt(max(1.0 - dot(mq, mq), 0.0));
    vec3 moon = vec3(0.98, 0.96, 0.88) * (1.0 - 0.3 * seas) * (0.75 + 0.25 * limb) * 1.25;
    // its phase: lit right of the terminator (an ellipse across the disk), the rest by earthshine
    float term = (1.0 - 2.0 * sky_kind.y) * sqrt(max(1.0 - mq.y * mq.y, 0.0));
    moon *= mix(0.04, 1.0, smoothstep(term - 0.08, term + 0.08, -mq.x));
    // a full moon: brighter and a wider halo; the eclipse: Earth's shadow, a dim copper red
    float full = step(0.99, sky_kind.y) * (1.0 - sky_kind.z);
    moon *= 1.0 + 0.25 * full;
    vec3 blood = vec3(0.62, 0.17, 0.07) * (0.55 + 0.45 * limb) * (1.0 - 0.35 * seas) *
                 (0.7 + 0.3 * smoothstep(-1.0, 1.0, mq.x + mq.y));
    moon = mix(moon, blood, sky_kind.z);
    float halo = (pow(md, 60.0) * 0.25 + pow(md, 12.0) * 0.06) * (1.0 + 0.8 * full) * (1.0 - 0.6 * sky_kind.z);
    add = mix(add, moon, disk * mclear * smoothstep(0.0, 0.05, up));
    add += mix(vec3(0.6, 0.68, 0.85), vec3(0.7, 0.25, 0.15), sky_kind.z) * halo * (0.4 + 0.6 * mclear);
  }
  // a city's lights on the low sky
  add += vec3(1.0, 0.55, 0.25) * city_glow * 0.06 * (1.0 - smoothstep(-0.05, 0.4, up));
  return add * clouds.w;
}

// the sun's color by its height, the moon's at night
vec3 sun_color() {
  vec3 col = mix(vec3(1.0, 0.5, 0.28), vec3(1.0, 0.96, 0.9), smoothstep(0.05, 0.35, sky_sun.w));
  // the red sunset: the low sun through a thick sky, deep orange-red and strong
  col = mix(col, vec3(1.25, 0.36, 0.1), sky_kind.w * (1.0 - smoothstep(0.1, 0.45, sky_sun.w)));
  return mix(col, vec3(0.13, 0.15, 0.22), clouds.w);
}

// our sky by day: the horizon in the fog's color (so the distance melts into it), deepening to blue
// overhead, a glow and a disk for the sun; greyer the more overcast. Sunny: a deep summer blue.
vec3 sky_color(vec3 dv) {
  vec3 wd = world_dir(dv);
  float up = max(wd.y, 0.0);
  vec3 zen = mix(vec3(0.2, 0.4, 0.78), vec3(0.08, 0.32, 0.9), sunny);
  vec3 hor = mix(fog_color * 1.05, vec3(0.6, 0.8, 0.97), sunny * 0.8);
  vec3 col = mix(hor, zen, pow(smoothstep(0.0, 0.75, up), 0.7));
  // overcast: one grey
  float over = clamp((clouds.y - 0.5) * 2.0, 0.0, 1.0);
  col = mix(col, vec3(luma(hor)) * vec3(0.9, 0.94, 1.0) * (1.0 - 0.35 * weather2.x), over);
  float sd = max(dot(dv, sky_sun.xyz), 0.0);
  vec3 sc = sun_color();
  // sunsets: a warm band along the horizon, wider and brighter toward the sun; the red one burns
  // orange-red low down under a mauve sky (the sun's half of the sky the most)
  {
    float low = 1.0 - smoothstep(0.1, 0.45, sky_sun.w);
    float toward = 0.5 + 0.5 * dot(normalize(vec2(wd.x, wd.z) + 1e-4),
                                   normalize(world_dir(sky_sun.xyz).xz + 1e-4));
    float band = exp(-up * (6.0 - 3.0 * toward)) * (0.35 + 0.65 * toward * toward);
    vec3 warm = mix(vec3(1.0, 0.62, 0.32), vec3(1.0, 0.3, 0.1), sky_kind.w);
    vec3 above = mix(col, vec3(0.42, 0.24, 0.36), 0.75 * sky_kind.w * (1.0 - band));
    col = mix(col, above, low);
    col = mix(col, warm * (0.8 + 0.5 * sky_kind.w), band * low * (0.35 + 0.55 * sky_kind.w) * (1.0 - over * 0.6));
  }
  // the desert's late afternoon: a pale lilac sky over a cream glow that floods the horizon on the
  // sun's side
  if (golden > 0.0) {
    vec3 lilac = mix(vec3(0.9, 0.74, 0.62), vec3(0.72, 0.68, 0.8), smoothstep(0.02, 0.3, up));
    col = mix(col, lilac, golden * 0.75 * (1.0 - over));
    // (the glow low on the horizon under the sun, wherever it stands: the light across the valley)
    vec2 sh = normalize(world_dir(sky_sun.xyz).xz + 1e-4);
    float az = max(dot(normalize(wd.xz + 1e-4), sh), 0.0);
    float glow = pow(az, 4.0) * exp(-up * 7.0) + 0.4 * pow(az, 16.0) * exp(-up * 3.0);
    col += vec3(1.0, 0.68, 0.46) * glow * golden * 0.5 * (1.0 - over * 0.7);
    col += sc * pow(sd, 16.0) * 0.3 * golden * (1.0 - over * 0.7);
  }
  col += sc * (pow(sd, 6.0) * 0.18 + pow(sd, 60.0) * 0.35) * (1.0 - over * 0.7);
  col += sc * smoothstep(0.99955, 0.9998, sd) * 3.0 * (1.0 - over);
  return col;
}

// the cloud volume: a slab 1100-2000 m up, marched along the view ray (view space, unit). Rounded
// heaps of 3D noise (the sky cover sets how much of it is cloud), drifting with the wind; each step
// lit by the sun through the cloud above it toward the sun (two samples), forward scattering bright
// around the sun, the shaded undersides in the sky's own color, and lit from inside by lightning.
// Returns the light it adds (rgb) and what still shows through (a).
float cloud_density(vec3 p, float lo, int octaves) {
  float h = (p.y - 1100.0) / 900.0;
  if (h <= 0.0 || h >= 1.0) {
    return 0.0;
  }
  vec3 q = p / 1300.0 + vec3(clouds.z * 0.0045, 0.0, clouds.z * 0.002);
  float n = vnoise3(q) * 0.6 + vnoise3(q * 2.6 + 7.0) * 0.27;
  if (octaves > 2) {
    n += vnoise3(q * 7.3 + 3.0) * 0.13;
  } else {
    n += 0.065;
  }
  // flat bottoms, heaped tops
  float shape = smoothstep(0.0, 0.12, h) * (1.0 - smoothstep(0.35, 1.0, h));
  // (the late afternoon's heaps with crisper edges: cumulus, clear sky between)
  return clamp((n * shape - lo) * (4.0 + 4.0 * golden), 0.0, 1.0);
}

vec4 cloud_volume(vec3 dv, float jitter, vec3 sky) {
  vec3 wd = world_dir(dv);
  if (wd.y < 0.015) {
    return vec4(0.0, 0.0, 0.0, 1.0);
  }
  float cam_y = cam_pos.y / 4096.0;
  float t0 = max((1100.0 - cam_y) / wd.y, 0.0);
  float t1 = min((2000.0 - cam_y) / wd.y, t0 + 7000.0);
  if (t0 > 30000.0) {
    return vec4(0.0, 0.0, 0.0, 1.0);
  }
  float lo = 0.6 - clouds.y * 0.42;
  vec3 sw = world_dir(sky_sun.xyz);
  vec3 sc = sun_color() * (1.0 - 0.6 * weather2.x);
  float cosang = dot(wd, sw);
  // two-lobe phase: a bright rim around the sun, some back scattering
  float phase = 0.6 + 1.6 * pow(max(cosang, 0.0), 6.0) + 0.25 * pow(max(-cosang, 0.0), 2.0);
  vec3 under = mix(sky * 0.55, vec3(luma(sky)) * 0.5, 0.3) * (1.0 - 0.45 * weather.x - 0.3 * weather2.x);
  vec3 top = sky * 0.9 + 0.08;
  // a warm hazy area (the desert): sunset clouds, gold where the sun lights them, dusty mauve
  // undersides and peach tops
  if (sun_haze > 0.0) {
    sc *= mix(vec3(1.0), vec3(1.2, 0.86, 0.52), sun_haze * 0.75);
    under = mix(under, vec3(0.5, 0.36, 0.38) * max(luma(under) * 2.2, 0.25 * (1.0 - clouds.w)), sun_haze * 0.55);
    top = mix(top, vec3(0.95, 0.72, 0.55), sun_haze * 0.35);
    // (and late in the day their shaded undersides a deep warm brown, the sun's gold on the rims)
    under = mix(under, vec3(0.3, 0.2, 0.15), golden * 0.9);
    top = mix(top, vec3(0.7, 0.52, 0.4), golden * 0.5);
    sc *= mix(vec3(1.0), vec3(1.3, 1.0, 0.7), golden);
  }
  float near_bolt = exp(-pow(mod(atan(wd.x, wd.z) - bolt.x + 3.14159, 6.28318) - 3.14159, 2.0) / 0.2);
  vec3 flash = vec3(0.8, 0.85, 1.0) * (weather2.w * (0.12 + 1.1 * near_bolt) + sheet * 0.9);
  // more steps toward the horizon, where the ray crosses kilometers of slab: long steps there
  // left a per-pixel grain (the jitter) the TAA couldn't smooth, drawn as fine diagonal hatching
  int N = int(mix(34.0, 16.0, clamp(wd.y * 2.5, 0.0, 1.0)));
  float dt = (t1 - t0) / float(N);
  vec3 sum = vec3(0.0);
  float trans = 1.0;
  for (int i = 0; i < N; i++) {
    vec3 p = cam_pos / 4096.0 + wd * (t0 + (float(i) + jitter) * dt);
    float d = cloud_density(p, lo, 3);
    if (d > 0.002) {
      float dl = cloud_density(p + sw * 90.0, lo, 2) * 90.0 + cloud_density(p + sw * 300.0, lo, 2) * 210.0;
      float lit = exp(-dl * 0.012);
      float powder = 1.0 - exp(-d * 4.0);
      float h = (p.y - 1100.0) / 900.0;
      vec3 light = sc * lit * phase * (0.5 + 0.5 * powder) * 1.15 + mix(under, top, h) + flash * (1.0 - h * 0.5);
      // (thinner by night: under the moon they read as dark cut-out slabs over the dim sky)
      // (the late afternoon's heaps solid: dark bellies, not veils)
      float a = 1.0 - exp(-d * 0.022 * (1.0 + 1.2 * golden) * dt * (1.0 - 0.45 * max(clouds.w, 1.0 - smoothstep(0.08, 0.3, luma(sky)))));
      sum += trans * a * light;
      trans *= 1.0 - a;
      if (trans < 0.02) {
        break;
      }
    }
  }
  // far clouds melt into the haze at the horizon
  float haze = 1.0 - exp(-t0 / 14000.0);
  sum = mix(sum, (1.0 - trans) * sky, haze);
  // by night they keep close to the sky's color, a little lighter (moonlit), never blacker than it
  // (the dawn's and dusk's dim skies too: the game doesn't call those night)
  float dim = max(clouds.w, 1.0 - smoothstep(0.08, 0.3, luma(sky)));
  sum = mix(sum, (1.0 - trans) * sky * 1.15, 0.7 * dim);
  return vec4(sum, trans);
}

// the clouds at a quarter of the pixels (cloud_pass): a = -1 where no sky shows. Read back
// bilinearly from only the texels that hold clouds (they stop at the scene's edges)
vec4 cloud_fetch(vec2 uv) {
  vec2 size = vec2(textureSize(tex_T15, 0));
  vec2 g = uv * size - 0.5;
  ivec2 i0 = ivec2(floor(g));
  vec2 f = g - vec2(i0);
  vec4 sum = vec4(0.0);
  float wsum = 0.0;
  for (int k = 0; k < 4; k++) {
    ivec2 o = ivec2(k & 1, k >> 1);
    vec4 s = texelFetch(tex_T15, clamp(i0 + o, ivec2(0), ivec2(size) - 1), 0);
    float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
    if (s.a >= 0.0 && w > 0.0) {
      sum += s * w;
      wsum += w;
    }
  }
  return wsum > 1e-4 ? sum / wsum : vec4(0.0, 0.0, 0.0, -1.0);
}

// the game's own sky around a point, broadly: its average over a wide ring of sky pixels (its
// painted cloud cards drift through it: read pixel by pixel they printed their cut-out shapes into
// our clouds' color); our sky where none of the ring is sky
vec3 game_sky_around(vec2 uv, vec3 fallback, vec2 radius) {
  vec3 sum = vec3(0.0);
  float n = 0.0;
  for (int k = 0; k < 12; k++) {
    float a = float(k) * 2.39996;
    float r = k < 6 ? 0.45 : 1.0;
    vec2 q = clamp(uv + r * radius * vec2(cos(a), sin(a) * (texel.x / texel.y)), vec2(0.002), vec2(0.998));
    float zz = view_z(texture(tex_T3, q).r);
    if (zz == 0.0 || zz > 2800.0 * 4096.0) {
      sum += texture(tex_T0, q).rgb;
      n += 1.0;
    }
  }
  return n > 0.0 ? sum / n : fallback;
}

// the cloud pass: one pixel here covers 2x2 of the screen. Marched only where the sky shows in
// one of them, then blended into last frame's clouds, found where this direction was then (the
// clouds are far enough to ignore the camera's move): the march's noise averages out over frames.
vec4 cloud_march() {
  vec2 uv = tex_coord;
  vec3 ray = normalize(vec3((uv.x * 2.0 - 1.0) / proj.z, (uv.y * 2.0 - 1.0) / proj.w, 1.0));
  ivec2 base = ivec2(gl_FragCoord.xy) * 2;
  ivec2 top = textureSize(tex_T3, 0) - 1;
  bool sky = false;
  for (int k = 0; k < 4; k++) {
    float zz = view_z(texelFetch(tex_T3, min(base + ivec2(k & 1, k >> 1), top), 0).r);
    sky = sky || zz == 0.0 || zz > 2800.0 * 4096.0;
  }
  if (!sky || world_dir(ray).y < 0.015) {
    return vec4(0.0, 0.0, 0.0, -1.0);
  }
  // the sky behind them as the composite makes it (by night the game's own, read from the scene)
  vec3 s = sky_color(ray);
  // (at dawn and dusk too: the game's dim sky, not ours, or the clouds come out darker than it)
  float dim = max(clouds.w, 1.0 - smoothstep(0.08, 0.3, luma(s)));
  vec3 gs = dim > 0.0 ? game_sky_around(uv, s, vec2(0.22)) : s;
  vec4 cl = cloud_volume(ray, pixel_angle() / 6.2832, mix(s, mix(gs, s, 0.25), dim));
  if (cloud_info.y > 0.0) {
    vec3 pp = to_prev * ray;
    vec2 prev_uv = 0.5 * motion.xy * pp.xy / pp.z + 0.5;
    if (pp.z > 0.0 && all(greaterThan(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)))) {
      vec4 h = cloud_fetch(prev_uv);
      // (lightning changes them at once)
      float keep = cloud_info.z * (1.0 - 0.8 * clamp((weather2.w + sheet) * 4.0, 0.0, 1.0));
      if (h.a >= 0.0) {
        cl = mix(cl, h, keep);
      }
    }
  }
  return cl;
}

// rain: real drops in the world. Each cell of a horizontal grid holds a column where drops fall
// (one every few meters, each a short streak, as a camera sees it); the view ray walks the cells
// it crosses (2D DDA) and meets each column where it passes closest. So the drops stay put in the
// world as the camera moves, the near ones pass by bigger, and the scene hides the ones behind it.
// o: the camera (m), d: the ray (world, unit), far: where the scene is along it (m), px: the angle
// of a pixel (radians). Returns how much rain covers the pixel.
float rain_grid(vec3 o, vec3 d, float near, float far, float cell, float px, float seed) {
  // the wind slants the rain: shear space so the drops' paths stand vertical in it
  vec2 slant = dust_wind.xy * (0.06 + 0.12 * weather2.x);
  o.xz -= slant * o.y;
  d.xz -= slant * d.y;
  vec2 dh = d.xz;
  float hl = length(dh);
  if (hl < 1e-4) {
    return 0.0;
  }
  vec2 start = o.xz + dh * near;
  vec2 c = floor(start / cell);
  vec2 stp = sign(dh);
  vec2 inv = 1.0 / max(abs(dh), vec2(1e-5));
  vec2 next = ((c + max(stp, 0.0)) * cell - o.xz) * stp * inv;  // ray t of the next cell edges
  float sum = 0.0;
  float t = near;
  float t_rain = weather.z;
  for (int i = 0; i < 28; i++) {
    if (t > far) {
      break;
    }
    float h = hash1(c + seed);
    // gusts: denser and sparser sheets of rain drifting with the wind
    vec2 cw = c * cell;
    float gust = 0.55 + 0.45 * smoothstep(-0.3, 0.4, gnoise(cw * 0.06 - dust_wind.xy * t_rain * 0.25));
    if (h < 0.92 * gust) {
      // the column, somewhere in the cell
      vec2 col = (c + 0.15 + 0.7 * vec2(fract(h * 13.7), fract(h * 71.3))) * cell;
      float tc = dot(col - o.xz, dh) / (hl * hl);  // closest along the ray
      if (tc > near && tc < far) {
        float miss = length(o.xz + dh * tc - col);
        float foot = tc * px;           // a pixel's size there (m)
        float width = max(0.002, foot * 0.7);
        float line = 1.0 - smoothstep(0.0, width, miss);
        if (line > 0.0) {
          // drops every 1.5 m down the column, falling 9 m/s, each 0.4 m long (motion blur)
          float y = o.y + d.y * tc;
          float v = fract((y + t_rain * 9.0) / 1.5 + fract(h * 37.1));
          float len = 0.35 / 1.5;
          float streak = smoothstep(0.0, len * 0.3, v) * (1.0 - smoothstep(len * 0.6, len, v));
          // thin far drops are faint, not thick
          // (not right at the lens: a drop there would cross the whole screen)
          sum += line * streak * min(1.0, 0.005 / width) * 3.0 * smoothstep(2.0, 3.5, tc);
        }
      }
    }
    // the next cell
    if (next.x < next.y) {
      t = next.x;
      next.x += cell * inv.x;
      c.x += stp.x;
    } else {
      t = next.y;
      next.y += cell * inv.y;
      c.y += stp.y;
    }
  }
  return sum;
}

float rain_drops(vec3 ray_view, float scene_z, float px) {
  vec3 d = world_dir(ray_view);
  vec3 o = cam_pos / 4096.0;
  // along the ray to the scene (m); the sky is far
  float far = scene_z > 0.0 ? scene_z / 4096.0 / max(ray_view.z, 1e-3) : 1e4;
  // near drops on a fine grid, farther ones on a coarser one
  return rain_grid(o, d, 2.0, min(far, 4.0), 0.25, px, 0.0) +
         rain_grid(o, d, 4.0, min(far, 12.0), 0.7, px, 17.0) * 0.9 +
         rain_grid(o, d, 12.0, min(far, 30.0), 1.6, px, 41.0) * 0.7;
}

// snowflakes: like the rain (columns in world cells, walked along the view ray), but round flakes
// drifting down at about a meter a second, swaying, carried by the wind
float snow_grid(vec3 o, vec3 d, float near, float far, float cell, float px, float seed) {
  float t_snow = weather.z;
  vec2 slant = dust_wind.xy * 0.35;
  o.xz -= slant * o.y;
  d.xz -= slant * d.y;
  vec2 dh = d.xz;
  float hl = length(dh);
  if (hl < 1e-4) {
    return 0.0;
  }
  vec2 start = o.xz + dh * near;
  vec2 c = floor(start / cell);
  vec2 stp = sign(dh);
  vec2 inv = 1.0 / max(abs(dh), vec2(1e-5));
  vec2 next = ((c + max(stp, 0.0)) * cell - o.xz) * stp * inv;
  float sum = 0.0;
  float t = near;
  for (int i = 0; i < 24; i++) {
    if (t > far) {
      break;
    }
    float h = hash1(c + seed);
    if (h < 0.85) {
      // the column sways as a whole (its flakes are far apart)
      vec2 sway = 0.18 * cell * vec2(sin(t_snow * 0.9 + h * 40.0), cos(t_snow * 0.7 + h * 23.0));
      vec2 col = (c + 0.2 + 0.6 * vec2(fract(h * 13.7), fract(h * 71.3))) * cell + sway;
      float tc = dot(col - o.xz, dh) / (hl * hl);
      if (tc > near && tc < far) {
        float miss = length(o.xz + dh * tc - col);
        float foot = tc * px;
        float r = max(0.012, foot * 0.9);
        if (miss < r) {
          // a flake every 0.9 m down the column, falling 1.1 m/s
          float y = o.y + d.y * tc;
          float dy = (fract((y + t_snow * 1.1) / 0.9 + fract(h * 37.1)) - 0.5) * 0.9;
          float dist = length(vec2(miss, dy));
          float flake = 1.0 - smoothstep(r * 0.4, r, dist);
          // far flakes smaller than a pixel are faint; none right at the lens
          sum += flake * min(1.0, (0.012 / r) * (0.012 / r) * 4.0) * smoothstep(1.0, 2.0, tc);
        }
      }
    }
    if (next.x < next.y) {
      t = next.x;
      next.x += cell * inv.x;
      c.x += stp.x;
    } else {
      t = next.y;
      next.y += cell * inv.y;
      c.y += stp.y;
    }
  }
  return sum;
}

// dust motes in the headlights' beams: specks hanging in the air in columns (as the snowflakes),
// drifting slowly, each lit only while it's inside the cone of the nearest vehicle's lamps,
// glinting as it tumbles. Returns how much lit mote covers the pixel
float mote_grid(vec3 o, vec3 d, float near, float far, float cell, float px, float seed) {
  float t_m = weather.z;
  vec2 dh = d.xz;
  float hl = length(dh);
  if (hl < 1e-4) {
    return 0.0;
  }
  mat3 to_view = transpose(view_to_world);
  vec2 start = o.xz + dh * near;
  vec2 c = floor(start / cell);
  vec2 stp = sign(dh);
  vec2 inv = 1.0 / max(abs(dh), vec2(1e-5));
  vec2 next = ((c + max(stp, 0.0)) * cell - o.xz) * stp * inv;
  float sum = 0.0;
  float t = near;
  for (int i = 0; i < 24; i++) {
    if (t > far) {
      break;
    }
    float h = hash1(c + seed);
    if (h < 0.7) {
      vec2 drift = 0.3 * cell * vec2(sin(t_m * 0.21 + h * 40.0), cos(t_m * 0.17 + h * 23.0)) +
                   dust_wind.xy * 0.15 * cell * sin(t_m * 0.3 + h * 9.0);
      vec2 col = (c + 0.2 + 0.6 * vec2(fract(h * 13.7), fract(h * 71.3))) * cell + drift;
      float tc = dot(col - o.xz, dh) / (hl * hl);
      if (tc > near && tc < far) {
        float miss = length(o.xz + dh * tc - col);
        float r = max(0.01, tc * px * 1.2);
        if (miss < r) {
          // a mote every 0.3 m up the column, bobbing a few centimeters
          float y = o.y + d.y * tc;
          float ph = (y + 0.05 * sin(t_m * 0.8 + h * 31.0)) / 0.3 + fract(h * 37.1);
          float dy = (fract(ph) - 0.5) * 0.3;
          float dist = length(vec2(miss, dy));
          float mote = 1.0 - smoothstep(r * 0.4, r, dist);
          // (only some places up the column hold a mote: no lines of them)
          if (hash1(c * 1.37 + vec2(floor(ph), seed)) > 0.45) {
            mote = 0.0;
          }
          if (mote > 0.0) {
            vec3 wp = vec3(col.x, y - dy, col.y);
            vec3 vp = to_view * (wp * 4096.0 - cam_pos);
            float lit = 0.0;
            for (int s = 0; s < min(spot_count, 2); s++) {
              vec3 q = vp - spot_pos[s].xyz;
              float dq = length(q);
              float dm = dq / 4096.0;
              lit = max(lit, smoothstep(0.88, 0.97, dot(q / max(dq, 1.0), spot_dir[s].xyz)) * spot_pos[s].w /
                                 (1.0 + dm * dm / 40.0) * smoothstep(0.4, 1.5, dm));
            }
            float glint = 0.55 + 0.45 * sin(t_m * (2.0 + 3.0 * fract(h * 5.3)) + floor(ph) * 7.1 + h * 50.0);
            // specks smaller than a pixel are faint
            sum += mote * lit * glint * min(1.0, (0.01 / r) * (0.01 / r) * 4.0);
          }
        }
      }
    }
    if (next.x < next.y) {
      t = next.x;
      next.x += cell * inv.x;
      c.x += stp.x;
    } else {
      t = next.y;
      next.y += cell * inv.y;
      c.y += stp.y;
    }
  }
  return sum;
}

float beam_motes(vec3 ray_view, float scene_z, float px) {
  vec3 d = world_dir(ray_view);
  vec3 o = cam_pos / 4096.0;
  float far = scene_z > 0.0 ? scene_z / 4096.0 / max(ray_view.z, 1e-3) : 1e4;
  return mote_grid(o, d, 0.8, min(far, 8.0), 0.35, px, 11.0) +
         mote_grid(o, d, 8.0, min(far, 22.0), 0.8, px, 37.0);
}

float snow_flakes(vec3 ray_view, float scene_z, float px) {
  vec3 d = world_dir(ray_view);
  vec3 o = cam_pos / 4096.0;
  float far = scene_z > 0.0 ? scene_z / 4096.0 / max(ray_view.z, 1e-3) : 1e4;
  return snow_grid(o, d, 1.0, min(far, 6.0), 0.45, px, 5.0) +
         snow_grid(o, d, 6.0, min(far, 18.0), 1.1, px, 29.0) * 0.8 +
         snow_grid(o, d, 18.0, min(far, 45.0), 2.4, px, 53.0) * 0.6;
}

// raindrops in the puddles: each 0.5 m cell gets a ring now and then, spreading and fading
// (returns the slope it gives the water, xz)
// how much of a puddle is at a world point (m), as the wet ground draws them (flat ground)
float puddle_at(vec2 xz) {
  float pd = gnoise(xz * 0.22) + 0.45 * gnoise(xz * 0.7 + 7.1) + 0.12 * gnoise(xz * 3.1 + 2.3);
  float spread = 0.22 * weather2.x - 0.3 * (1.0 - puddle_level);
  return smoothstep(-0.12 - spread, 0.02 - spread, pd) * max(weather.w, puddle_level);
}

// rings on the puddles where Jak stepped: spreading 0.8 m/s for a second (slope, xz)
vec2 step_ripples(vec2 xz) {
  vec2 slope = vec2(0.0);
  for (int i = 0; i < splash_count; i++) {
    float strength = floor(splash[i].w / 10.0);
    float age = splash[i].w - 10.0 * strength;
    vec2 rel = xz - splash[i].xz / 4096.0;
    float d = length(rel);
    for (int k = 0; k < 4; k++) {
      float a = age - float(k) * 0.12;
      if (a <= 0.0) {
        continue;
      }
      float ring = d - a * (0.8 + 0.3 * strength);
      float band = exp(-ring * ring / 0.0012) * (1.0 - smoothstep(0.3, 1.1, a));
      slope += rel / max(d, 1e-3) * band * sin(ring * 90.0) * 0.38 * strength;
    }
  }
  return slope;
}

// the drops a step throws up from a puddle: ten (twenty landing) small drops on short arcs,
// hidden behind the scene. Returns how much drop covers the pixel
float step_drops(vec2 uv, float scene_z) {
  float sum = 0.0;
  mat3 to_view = transpose(view_to_world);
  for (int i = 0; i < splash_count; i++) {
    float strength = floor(splash[i].w / 10.0);
    float age = splash[i].w - 10.0 * strength;
    if (age > 0.55) {
      continue;
    }
    vec3 foot = splash[i].xyz / 4096.0;  // m
    // (a raindrop's bounce, strength 0: anywhere in the rain)
    bool rain_drop = strength < 0.5;
    float wet_here = rain_drop ? 1.0 : puddle_at(foot.xz);
    if (wet_here < 0.2) {
      continue;
    }
    // skip what's far from this pixel on screen
    vec3 cv = to_view * (splash[i].xyz - cam_pos);
    if (cv.z < 0.5 * 4096.0) {
      continue;
    }
    vec2 cuv = 0.5 + 0.5 * vec2(cv.x * proj.z, cv.y * proj.w) / cv.z;
    float reach = 1.2 * 4096.0 * abs(proj.w) / cv.z;  // ~1.2 m in uv (height)
    if (abs(uv.y - cuv.y) > reach || abs(uv.x - cuv.x) > reach * abs(proj.z / proj.w)) {
      continue;
    }
    int n = rain_drop ? 3 : (strength > 1.5 ? 20 : 10);
    for (int j = 0; j < n; j++) {
      float h1 = hash1(vec2(float(j), splash[i].x + 0.37));
      float h2 = hash1(vec2(splash[i].z, float(j) * 1.7));
      float h3 = hash1(vec2(float(j) * 3.1, splash[i].y));
      float ang = (float(j) + h1) * 6.2832 / float(n);
      float speed = rain_drop ? 0.15 + 0.3 * h2 : (0.5 + 0.9 * h2) * (0.7 + 0.3 * strength);
      float up = rain_drop ? 0.6 + 0.7 * h3 : (1.4 + 1.2 * h3) * (0.8 + 0.2 * strength);
      float t = age;
      vec3 wp = foot + vec3(cos(ang) * speed * t, up * t - 4.9 * t * t, sin(ang) * speed * t);
      if (wp.y < foot.y - 0.02) {
        continue;  // back in the water
      }
      vec3 v = to_view * (wp * 4096.0 - cam_pos);
      if (v.z < 0.3 * 4096.0 || (scene_z > 0.0 && v.z > scene_z + 0.05 * 4096.0)) {
        continue;
      }
      vec2 duv = 0.5 + 0.5 * vec2(v.x * proj.z, v.y * proj.w) / v.z;
      vec2 dd = (uv - duv) / texel;
      // a drop of ~2 cm, at least a pixel and a bit, a little stretched along its fall
      float rad = max(rain_drop ? 1.0 : 1.3,
                      (rain_drop ? 0.01 : 0.018) * 4096.0 * abs(proj.w) * 0.5 / v.z / texel.y);
      float k = 1.0 - smoothstep(rad * 0.5, rad, length(vec2(dd.x, dd.y * 0.6)));
      sum += k * (1.0 - smoothstep(0.3, 0.55, age)) * min(1.0, wet_here * 2.0);
    }
  }
  return min(sum, 1.0);
}

vec2 rain_ripples(vec2 xz, float t) {
  vec2 slope = vec2(0.0);
  vec2 cell = floor(xz / 0.5);
  for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
      vec2 cc = cell + vec2(i, j);
      float h = hash1(cc);
      vec2 center = (cc + 0.2 + 0.6 * vec2(h, fract(h * 23.1))) * 0.5;
      float cycle = t * 1.3 + h * 7.0;
      float phase = fract(cycle);
      // not every cell every time
      if (hash1(cc + floor(cycle) * 1.37) > 0.45) {
        continue;
      }
      vec2 rel = xz - center;
      float d = length(rel);
      float ring = d - phase * 0.3;
      float band = exp(-ring * ring / 0.0005) * (1.0 - phase) * (1.0 - phase);
      slope += rel / max(d, 1e-3) * band * sin(ring * 140.0) * 0.5;
    }
  }
  return slope;
}

// the little crowns the drops of rain_ripples throw up as they land, at the start of each ring
// (same cells and timing): how much splash is at a point. radius in m (bigger far away, so it
// still covers a pixel)
float rain_splashes(vec2 xz, float t, float radius) {
  float sum = 0.0;
  vec2 cell = floor(xz / 0.5);
  for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
      vec2 cc = cell + vec2(i, j);
      float h = hash1(cc);
      vec2 center = (cc + 0.2 + 0.6 * vec2(h, fract(h * 23.1))) * 0.5;
      float cycle = t * 1.3 + h * 7.0;
      float phase = fract(cycle);
      if (phase > 0.14 || hash1(cc + floor(cycle) * 1.37) > 0.45) {
        continue;
      }
      vec2 rel = xz - center;
      sum += exp(-dot(rel, rel) / (radius * radius)) * (1.0 - phase / 0.14);
    }
  }
  return sum;
}

// distant rain hitting a surface: one short sparkle per cell for each drop, the cells growing
// with distance (powers of two, blended) so the drops stay about a pixel big and as many on screen
float rain_spark_level(vec2 xz, float t, float cs, float r) {
  float sum = 0.0;
  vec2 cell = floor(xz / cs);
  for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
      vec2 cc = cell + vec2(i, j);
      float h = hash1(cc + cs * 17.0);
      vec2 center = (cc + 0.2 + 0.6 * vec2(h, fract(h * 23.1))) * cs;
      float cycle = t * 1.6 + h * 7.0;
      float phase = fract(cycle);
      if (phase > 0.12 || hash1(cc + floor(cycle) * 1.37 + cs) > 0.55) {
        continue;
      }
      vec2 rel = xz - center;
      sum += exp(-dot(rel, rel) / (r * r)) * (1.0 - phase / 0.12);
    }
  }
  return sum;
}

// across: the pixel's width there (m)
float rain_sparkles(vec2 xz, float t, float across) {
  float r = max(across, 0.02);
  float lvl = max(log2(r * 4.5 / 0.25), 0.0);
  float l0 = floor(lvl);
  float cs = 0.25 * exp2(l0);
  return mix(rain_spark_level(xz, t, cs, r), rain_spark_level(xz, t, cs * 2.0, r), lvl - l0);
}

// FXAA (Lottes' compact version): blend along the edge direction where the luma jumps
vec3 fxaa_at(vec2 uv) {
  vec3 m = texture(tex_T0, uv).rgb;
  vec3 nw = texture(tex_T0, uv + vec2(-1.0, -1.0) * texel).rgb;
  vec3 ne = texture(tex_T0, uv + vec2(1.0, -1.0) * texel).rgb;
  vec3 sw = texture(tex_T0, uv + vec2(-1.0, 1.0) * texel).rgb;
  vec3 se = texture(tex_T0, uv + vec2(1.0, 1.0) * texel).rgb;
  float lm = luma(m), lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se);
  float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
  float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
  if (lmax - lmin < max(0.04, lmax * 0.12)) {
    return m;  // no edge
  }
  vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
  float reduce = max((lnw + lne + lsw + lse) * 0.03125, 1.0 / 128.0);
  float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
  dir = clamp(dir * rcp_min, vec2(-8.0), vec2(8.0)) * texel;
  vec3 a = 0.5 * (texture(tex_T0, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                  texture(tex_T0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
  vec3 b = a * 0.5 + 0.25 * (texture(tex_T0, uv - dir * 0.5).rgb + texture(tex_T0, uv + dir * 0.5).rgb);
  float lb = luma(b);
  return (lb < lmin || lb > lmax) ? a : b;
}

// the occlusion and the characters' sun shadows, made at half the resolution a side: of the four
// texels around, those at this pixel's depth (plain bilinear blurred them across the edges of
// grass blades and pebbles, and the TAA's jitter moved those edges each frame: they flickered)
float ao_upsample(vec2 uv, float zc) {
  vec2 size = vec2(textureSize(tex_T2, 0));
  vec2 g = uv * size - 0.5;
  ivec2 i0 = ivec2(floor(g));
  vec2 f = g - vec2(i0);
  if (zc <= 0.0) {
    return texture(tex_T2, uv).r;
  }
  float sum = 0.0;
  float wsum = 0.0;
  for (int k = 0; k < 4; k++) {
    ivec2 o = ivec2(k & 1, k >> 1);
    ivec2 ic = clamp(i0 + o, ivec2(0), ivec2(size) - 1);
    vec2 tuv = (vec2(ic) + 0.5) / size;
    float zs = view_z(texture(tex_T3, tuv).r);
    float bw = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
    float dz = abs(zs - zc) / (zc * 0.02 + 0.05 * 4096.0);
    float w = (bw + 1e-3) / (1.0 + dz * dz * 16.0);
    sum += texelFetch(tex_T2, ic, 0).r * w;
    wsum += w;
  }
  return sum / wsum;
}

void main() {
  if (cloud_pass == 1) {
    color = cloud_march();
    return;
  }
  vec2 uv = tex_coord;
  float d = texture(tex_T3, uv).r;
  float z = view_z(d);
  vec3 p = d > 1e-7 ? view_pos(uv, d) : vec3(0.0);
  // the view ray, and the angle a pixel spans
  vec3 ray = normalize(vec3((tex_coord.x * 2.0 - 1.0) / proj.z, (tex_coord.y * 2.0 - 1.0) / proj.w, 1.0));
  float pixel_rad = 2.0 * texel.y / abs(proj.w);
  float height = dot(up_view, p);  // above (+) or below (-) the camera
  // is the scene level here (a floor, a decal lying on the water)? (uniform control flow)
  vec3 scene_dp = view_pos(uv, max(d, 1e-6));
  bool scene_level = abs(dot(normalize(cross(dFdx(scene_dp), dFdy(scene_dp))), up_view)) > 0.9;

  // water: the near sea's surface in front of the scene, or the far sea still showing. From here
  // on the surface is this pixel's depth (fog, blur, ...); seabed_d keeps what's under it.
  float wsurf = 0.0;
  float seabed_d = d;
  bool water_deep = true;
  bool water_decal = false;  // something flat lying on the water (splash and ripple discs)
  if (water_strength > 0.0) {
    float wd = texture(tex_T5, uv).r;
    float fd = texture(tex_T6, uv).r;
    bool under_is_sea = fd > 1e-7 && abs(fd - d) < 1e-7 && dot(up_view, view_pos(uv, fd)) < -4096.0;
    // (where the scene's depth changed after the sea was drawn, something drawn later covers it,
    // as the game shows it: Spargus palace's floor, under the sea's level)
    bool covered = abs(texture(tex_T14, uv).r - d) > 1e-7;
    if (wd > d + 1e-7 && !covered) {
      wsurf = wd;
      water_deep = under_is_sea || d <= 1e-7;
    } else if (under_is_sea) {
      wsurf = fd;
    } else {
      // a flat effect a few cm over the water (splashes draw see-through discs that write depth):
      // still water, with the game's color only where it's brighter (the foam)
      // (the near sea only: the far one is a plane under the whole level, docks included)
      float ws = wd;
      // (level and up to 40 cm over it, or anything within 12 cm: the discs' edges; the sea must
      // be drawn behind it: where something solid hid the sea, its depth is the scene's own)
      if (ws > 1e-7 && d > ws + 1e-7) {
        float above = dot(up_view, p - view_pos(uv, ws));
        if (above > 0.0 && above < (scene_level ? 0.4 : 0.12) * 4096.0) {
          wsurf = ws;
          water_decal = true;
        }
      }
    }
    if (wsurf > 0.0) {
      d = wsurf;
      z = view_z(d);
      p = view_pos(uv, d);
      height = dot(up_view, p);
    }
  }
  // explosions: a shockwave ring racing out from each one bends the picture for half a second
  for (int i = 0; i < blast_count; i++) {
    vec3 bp = blast_pos[i].xyz;
    float age = blast_age[i];
    float ring = blast_pos[i].w * (1.0 + 14.0 * age);  // game units
    vec2 cuv = vec2(bp.x * proj.z / bp.z, bp.y * proj.w / bp.z) * 0.5 + 0.5;
    vec2 size = abs(vec2(proj.z, proj.w)) * 0.5 * ring / bp.z;  // the ring's radii in uv
    vec2 rel = tex_coord - cuv;
    float r = length(rel / size);
    float band = exp(-pow((r - 1.0) / 0.12, 2.0)) * (1.0 - smoothstep(0.1, 0.5, age));
    // a gun's blast: only the arc in front of the muzzle (as seen on screen; all round when it's
    // fired toward or away from the camera)
    if (blast_dir[i].w > 0.5) {
      vec3 ahead = bp + blast_dir[i].xyz * 4096.0;
      vec2 auv = vec2(ahead.x * proj.z / ahead.z, ahead.y * proj.w / ahead.z) * 0.5 + 0.5;
      vec2 sd = (auv - cuv) / size;
      float slen = length(sd);
      float front = dot(normalize(rel / size + 1e-6), sd / max(slen, 1e-6));
      // (a charged shot: a much wider arc)
      float arc = blast_dir[i].w > 2.5 ? -0.5 : 0.2;
      band *= mix(1.0, smoothstep(arc, arc + 0.5, front), clamp(slen * 3.0, 0.0, 1.0));
    }
    if (abs(blast_dir[i].w - 2.0) < 0.5) {
      continue;  // charging: no wave, no hot air
    }
    if (band > 0.001) {
      uv += normalize(rel + 1e-6) * band * 0.012 * blast_strength;
    }
    // then the hot air over the blast wobbles for a second
    if (blast_sparks > 0.0 && age < 1.3) {
      vec2 hsize = abs(vec2(proj.z, proj.w)) * 0.5 * blast_pos[i].w * 2.5 / bp.z;
      vec2 hr = rel / hsize;
      float heat = exp(-dot(hr, hr) * 1.5) * (1.0 - age / 1.3) * smoothstep(0.0, 0.15, age);
      if (heat > 0.001) {
        float ht = haze.w;
        uv += vec2(sin(hr.y * 14.0 - ht * 11.0), cos(hr.x * 12.0 - ht * 9.0)) * heat * 0.004 *
              blast_sparks;
      }
    }
  }

  // fires: the hot air over each flame bends what's seen through it
  vec2 fire_axis[4];  // the flame's up in uv, one flame size long
  vec2 fire_uv[4];
  for (int i = 0; i < fire_count; i++) {
    vec3 fp = fire_pos[i].xyz;
    float fs = fire_pos[i].w;
    vec3 top = fp + up_view * fs;
    fire_uv[i] = vec2(fp.x * proj.z / fp.z, fp.y * proj.w / fp.z) * 0.5 + 0.5;
    fire_axis[i] = vec2(top.x * proj.z / top.z, top.y * proj.w / top.z) * 0.5 + 0.5 - fire_uv[i];
    if (fp.z < 0.5 * 4096.0 || top.z < 0.5 * 4096.0 || fire.x <= 0.0) {
      continue;
    }
    vec2 a = fire_axis[i];
    float aa = max(dot(a, a), 1e-10);
    vec2 rel = tex_coord - fire_uv[i];
    float along = dot(rel, a) / aa;                      // flame sizes up
    float across = (rel.x * a.y - rel.y * a.x) / aa;     // and sideways
    float column = exp(-across * across * 1.2) * smoothstep(-0.3, 0.6, along) *
                   (1.0 - smoothstep(1.5, 4.5, along));
    // only what's behind the fire
    if (column > 0.0 && (z == 0.0 || z > fp.z - fs)) {
      float t = fire.z;
      vec2 wob = vec2(sin(along * 9.0 - t * 9.0 + across * 3.0) + 0.5 * sin(along * 21.0 - t * 14.0),
                      cos(along * 7.0 - t * 8.0 + across * 5.0));
      uv += wob * length(a) * fire.x * column;
    }
  }

  // the water's world position and pixel footprint, here in uniform control flow (derivatives)
  vec2 water_world = vec2(dot(right_view, p) + cam_pos.x, dot(fwd_view, p) + cam_pos.z) / 4096.0;
  float water_foot = clamp(length(fwidth(water_world)), 0.0005, 50.0);

  // the surface under the water: is it level (a beach) or upright (a leg, a wall)?
  vec3 seabed_p = view_pos(uv, max(seabed_d, 1e-6));
  vec3 seabed_n = normalize(cross(dFdx(seabed_p), dFdy(seabed_p)));
  bool seabed_level = abs(dot(seabed_n, up_view)) > 0.75;

  // heat haze: far ground below the camera wobbles a little
  if (haze.x > 0.0 && z > 0.0) {
    float amount = smoothstep(haze.y, haze.z, z) * clamp(-height / (z * 0.05 + 1.0), 0.0, 1.0);
    if (amount > 0.0) {
      float t = haze.w;
      vec2 wob = vec2(sin(uv.y * 180.0 + t * 3.1) + 0.5 * sin(uv.y * 410.0 - t * 4.3),
                      cos(uv.x * 140.0 + t * 2.3));
      uv += wob * haze.x * amount;
    }
  }

  // hot air over lava: the picture ripples, rising, stronger above the glow of the lava (the pixels
  // a little below this one)
  if (snow_heat.y > 0.0 && z > 0.0) {
    vec3 below = texture(tex_T0, uv - vec2(0.0, 0.035)).rgb;
    float lava = smoothstep(0.12, 0.35, below.r - below.b) * smoothstep(0.45, 0.85, below.r);
    float amount = snow_heat.y * smoothstep(2.0 * 4096.0, 15.0 * 4096.0, z) * (0.3 + 0.7 * lava);
    if (amount > 0.0) {
      float t = haze.w;
      vec2 wob = vec2(sin(uv.y * 230.0 - t * 6.0 + 2.0 * sin(uv.x * 70.0)),
                      sin(uv.x * 160.0 + uv.y * 90.0 - t * 5.0));
      uv += wob * 0.0018 * amount;
    }
  }

  // camera motion blur: where this pixel was last frame, if the world held still
  float blur = 0.0;
  vec3 c = vec3(0.0);
  if (motion.z > 0.0 && z > motion_keep.x && wsurf == 0.0) {
    vec3 pp = to_prev * p + to_prev_offset;
    vec2 prev_uv = 0.5 * vec2(motion.x * pp.x, motion.y * pp.y) / pp.z + 0.5;
    vec2 vel = (uv - prev_uv) * motion.z * smoothstep(motion_keep.x, motion_keep.y, z);
    float len = length(vel);
    if (len > motion.w) {
      vel *= motion.w / len;
      len = motion.w;
    }
    blur = clamp(len / (texel.x * 3.0), 0.0, 1.0);
    if (blur > 0.0) {
      vec3 sum = vec3(0.0);
      for (int i = 0; i < 8; i++) {
        sum += texture(tex_T0, uv + vel * (float(i) / 7.0 - 0.5)).rgb;
      }
      c = sum / 8.0;
    }
  }
  if (blur == 0.0) {
    c = fxaa != 0 ? fxaa_at(uv) : texture(tex_T0, uv).rgb;
  }

  // water: our own shading over the game's, the same for the near and far sea (drawn by two
  // renderers with different colors), with reflections, sun glints, clearer shallows and foam
  if (wsurf > 0.0) {
    // ripples: the slope of a non-repeating noise height field (finite differences)
    vec2 world = water_world;
    float t = haze.w;
    float far = 1.0 / (1.0 + z / (60.0 * 4096.0));
    float e = 0.08;
    float h0 = water_height(world, t, water_foot);
    vec2 slope = vec2(water_height(world + vec2(e, 0.0), t, water_foot) - h0,
                      water_height(world + vec2(0.0, e), t, water_foot) - h0) / e;

    // how deep the water is here (meters), from the sea bed seen through it (near sea only)
    float depth_m = 40.0;
    if (!water_deep && seabed_d > 1e-7) {
      depth_m = max(dot(up_view, p - seabed_p), 0.0) / 4096.0;
    }
    // a pond: murky, the bed gone a few cm down, no swell or caustics
    bool pond_here = pond != 0;
    float clear_water = exp(-depth_m / (pond_here ? 0.25 : 2.0));

    // the swell: long waves in open water, faded out with distance (they'd shimmer) and depth
    float swell_amt = (pond_here ? 0.0 : water_swell_strength) * smoothstep(1.0, 6.0, depth_m) *
                      (1.0 - smoothstep(0.3, 1.5, water_foot));
    if (swell_amt > 0.0) {
      float s0 = water_swell(world, t);
      slope += swell_amt * vec2(water_swell(world + vec2(0.5, 0.0), t) - s0,
                                water_swell(world + vec2(0.0, 0.5), t) - s0) / 0.5;
    }
    // wakes: rings spreading from where Jak touched the water lately (each point of his path near
    // this water's height)
    float water_y = dot(up_view, p) + cam_pos.y;
    float wake_foam = 0.0;
    for (int i = 0; i < wake_count; i++) {
      // (Jak's root is his feet: up to 2.5 m under the surface swimming, over it riding; a
      // vehicle's seat higher)
      bool ride = wake[i].w >= 10.0;
      float above = wake[i].y - water_y;
      if (above < -2.5 * 4096.0 || above > (ride ? 3.0 : 1.0) * 4096.0) {
        continue;
      }
      vec2 rel = world - wake[i].xz / 4096.0;  // meters
      float dist = length(rel);
      float age = wake[i].w - (ride ? 10.0 : 0.0);
      if (ride) {
        // churned water behind it: foam spreading and thinning out
        float fr = 0.9 + 1.2 * age;
        wake_foam += exp(-dist * dist / (fr * fr)) * (1.0 - age / 2.0) * 0.7;
      }
      float x = dist - (0.3 + 1.4 * age);
      float band = exp(-x * x / 0.15) * (1.0 - age / 2.0);
      if (band > 0.001) {
        slope += rel / max(dist, 0.05) * (band * 0.45 * wake_strength * sin(x * 10.0));
      }
    }
    // rain on the water: rings where the drops land (two staggered layers) and their small
    // splashes up close; further out, where the rings would shimmer, the surface just roughened
    float rain_hits = 0.0;
    if (weather.x > 0.01) {
      float rain_amt = min(weather.x, 1.5);
      float rings = 1.0 - smoothstep(0.03, 0.08, water_foot);
      vec2 w2 = world * 1.6 + vec2(3.7, 1.3);
      if (rings > 0.0) {
        slope += (rain_ripples(world, t) + rain_ripples(w2, t + 0.37)) * rain_amt * rings;
      }
      // the drops' splashes: crowns up close, sparkles further out (sized by the pixel's width, not
      // its length along the view, which grows at grazing angles), out to the horizon
      float across = z / 4096.0 * pixel_rad;
      float close = 1.0 - smoothstep(0.012, 0.03, across);
      if (close > 0.0) {
        rain_hits = (rain_splashes(world, t, 0.018) + rain_splashes(w2, t + 0.37, 0.03)) * close;
      }
      if (close < 1.0) {
        rain_hits += rain_sparkles(world, t, across) * 1.6 * (1.0 - close) *
                     (1.0 - smoothstep(350.0 * 4096.0, 600.0 * 4096.0, z));
      }
      rain_hits *= rain_amt;
      vec2 rw = world / clamp(water_foot * 4.0, 0.08, 2.0) + vec2(t * 2.0, -t * 1.7);
      slope += vec2(gnoise(rw), gnoise(rw + 5.3)) * 0.08 * rain_amt * (1.0 - rings) * far;
    }
    vec3 n = normalize(up_view + slope.x * right_view + slope.y * fwd_view);

    // what's under shallow water, bent by the waves (refraction), where the bent ray still lands
    // under water (not on something above the surface); lit by caustics where the sun gets in
    if (!water_deep && clear_water > 0.02) {
      vec2 ruv = uv + slope * water_refraction * min(depth_m, 2.0) * far;
      float rwd = texture(tex_T5, ruv).r;
      if (rwd > texture(tex_T3, ruv).r + 1e-7) {
        c = texture(tex_T0, ruv).rgb;
      }
      if (sun_dir.w > 0.0 && seabed_level && water_caustics > 0.0 && !pond_here) {
        vec2 bed = vec2(dot(right_view, seabed_p) + cam_pos.x, dot(fwd_view, seabed_p) + cam_pos.z) / 4096.0;
        float k = caustics(bed, t) * water_caustics * sun_dir.w * smoothstep(0.05, 0.4, depth_m) *
                  exp(-depth_m / 3.0) * (1.0 - smoothstep(0.3, 1.0, water_foot));
        c += c * k * vec3(0.9, 1.0, 0.95);
      }
    }
    vec3 vdir = normalize(p);
    vec3 r = reflect(vdir, n);
    float facing = clamp(dot(-vdir, n), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);

    // the water's body: one color for the near and far sea (the game draws them differently),
    // shaded a little by the ripples so it keeps some texture; clearer where it's shallow, where
    // what's under it (and the game's own water) shows through
    float shade = 1.0 + 0.3 * far * dot(n - up_view, normalize(fwd_view + right_view * 0.3));
    vec3 body = night_dim(mix(water_deep_color, water_shallow_color, clear_water) * shade);
    // (a pond's edge: its sandy bed seen through a few cm of murky water, not the sea's light
    // turquoise: that drew a pale line along the shore, there only once the near water is drawn)
    if (pond_here) {
      body = mix(night_dim(water_deep_color * shade), c * vec3(0.7, 0.76, 0.78), clear_water);
    }
    // (only a little of the game's own water texture shows: it's a small repeating tile)
    vec3 wc = mix(body, c, clear_water * 0.3 * water_unify + (1.0 - water_unify));

    // reflection. Far away: the screen where the reflected ray vanishes (sky, distant hills).
    // Near: trace the ray through the depth buffer (which has no water) to catch nearby things.
    vec3 refl = fog_color;
    if (r.z > 1e-3) {
      vec2 env_uv = clamp(0.5 * vec2(proj.z * r.x, proj.w * r.y) / r.z + 0.5, vec2(0.002), vec2(0.998));
      refl = texture(tex_T0, env_uv).rgb;
    }
    vec3 ssr = refl;
    float ssr_hit = 0.0;
    if (z < 80.0 * 4096.0) {
      float step_len = 0.25 * 4096.0 + z * 0.01;
      // start each pixel's march a little further along (the TAA averages the steps away: no
      // staircase where the reflection meets what it reflects)
      vec3 q = p + r * step_len * hash1(gl_FragCoord.xy + fract(haze.w * 7.0) * 61.0);
      for (int i = 0; i < 32; i++) {
        q += r * step_len;
        step_len *= 1.12;
        if (q.z <= 0.0) {
          break;  // came back toward the camera
        }
        vec2 quv = 0.5 * vec2(proj.z * q.x, proj.w * q.y) / q.z + 0.5;
        if (quv.x < 0.0 || quv.y < 0.0 || quv.x > 1.0 || quv.y > 1.0) {
          break;
        }
        float sd = texture(tex_T3, quv).r;
        float sfd = texture(tex_T6, quv).r;
        if (sd <= 1e-7 || (sfd > 1e-7 && abs(sfd - sd) < 1e-7)) {
          continue;  // sky, clouds or the sea itself: not a reflector
        }
        float scene_z = view_z(sd);
        if (q.z > scene_z && q.z - scene_z < step_len + 4096.0) {
          // home in on where the ray meets the surface (halving the last step)
          vec3 lo = q - r * step_len / 1.12;
          vec3 hi = q;
          for (int k = 0; k < 5; k++) {
            vec3 mid = (lo + hi) * 0.5;
            vec2 muv = 0.5 * vec2(proj.z * mid.x, proj.w * mid.y) / mid.z + 0.5;
            if (mid.z > view_z(texture(tex_T3, muv).r)) {
              hi = mid;
            } else {
              lo = mid;
            }
          }
          quv = 0.5 * vec2(proj.z * hi.x, proj.w * hi.y) / hi.z + 0.5;
          // fade toward the screen's edges, wider at the top (where the reflected ray leaves the
          // picture it ends in a hard line otherwise)
          vec2 edge = min(quv, 1.0 - quv);
          ssr_hit = smoothstep(0.0, 0.08, edge.x) * smoothstep(0.0, 0.15, edge.y) *
                    (1.0 - smoothstep(50.0 * 4096.0, 80.0 * 4096.0, z));
          ssr = texture(tex_T0, quv).rgb;
          break;
        }
      }
    }
    // what the screen doesn't have: traced through the levels (near water only, it's costly)
    // (faded out from 150 to 200 m, no seam)
    refl = reflection_with_rt(p, r, refl, ssr, ssr_hit, 400.0 * 4096.0,
                              1.0 - smoothstep(150.0 * 4096.0, 200.0 * 4096.0, z));
    wc = rt_mode == 3 ? refl : mix(wc, refl, clamp(fresnel * 1.2, 0.0, 1.0) * water_strength);

    // the lamps on the water at night: each one's image drawn out by the waves into a shimmering
    // column toward the eye (a harbor at night); it reaches much further than the lamp's light
    float lamp_night = max(night_dark.x, clouds.w);
    if ((light_count > 0 || spot_count > 0) && lamp_night > 0.0) {
      vec3 lsum = vec3(0.0);
      for (int i = 0; i < light_count; i++) {
        vec3 tl = light_pos[i].xyz - p;
        float d = length(tl);
        vec3 h = normalize(tl / max(d, 1.0) - vdir);
        float nh = max(dot(n, h), 0.0);
        float sp = pow(nh, 500.0) * 5.0 + pow(nh, 70.0) * 0.2;
        float rr = d / max(light_pos[i].w, 4096.0);
        lsum += light_color[i].rgb * sp / (1.0 + 0.04 * rr * rr);
      }
      for (int i = 0; i < spot_count; i++) {
        vec3 tl = spot_pos[i].xyz - p;
        float d = length(tl);
        vec3 l = tl / max(d, 1.0);
        vec3 h = normalize(l - vdir);
        float nh = max(dot(n, h), 0.0);
        // only toward where the headlight points (seen from the front it's blinding, from behind dark)
        float facing_us = smoothstep(0.3, 0.8, dot(-l, spot_dir[i].xyz));
        lsum += vec3(1.0, 0.93, 0.8) * (pow(nh, 500.0) * 6.0 + pow(nh, 70.0) * 0.25) * facing_us *
                spot_pos[i].w / (1.0 + d * d / (60.0 * 60.0 * 4096.0 * 4096.0));
      }
      wc += lsum * lamp_night * (0.4 + 0.6 * water_strength);
    }
    // the far lights too (across a harbor, the desert's fires by an oasis): thin columns of light
    // broken up by the waves
    for (int i = 0; i < beacon_count; i++) {
      vec3 l = normalize(beacon_pos[i].xyz - p);
      float nh = max(dot(n, normalize(l - vdir)), 0.0);
      float fade = exp(-length(beacon_pos[i].xyz) / (1800.0 * 4096.0));
      wc += beacon_color[i].rgb * beacon_color[i].w * fade * (pow(nh, 700.0) * 3.0 + pow(nh, 90.0) * 0.08) *
            (0.4 + 0.6 * water_strength);
    }

    // sun glints: a tight highlight plus a broad sheen toward the sun
    if (sun_dir.w > 0.0) {
      float sr = max(dot(r, sun_dir.xyz), 0.0);
      float glint = pow(sr, 900.0) * 7.0 + pow(sr, 80.0) * 0.25;
      wc += vec3(1.0, 0.93, 0.78) * glint * sun_dir.w;
    }

    // the drops' splashes, lit like the rain
    if (rain_hits > 0.0) {
      wc = mix(wc, wc * 1.3 + vec3(0.16, 0.17, 0.19) + fog_color * 0.4, min(rain_hits, 1.0) * 0.8);
    }

    // foam where the sea meets the shore, moving with the waves (a still pond has none)
    if (!water_deep && seabed_level && depth_m < 0.8 && !pond_here) {
      float wave = 0.5 + 0.7 * gnoise(world * 1.4 + vec2(t * 0.35, -t * 0.2)) +
                   0.35 * gnoise(world * 3.7 - vec2(t * 0.5, t * 0.3));
      float foam = smoothstep(0.8, 0.05, depth_m) * smoothstep(0.25, 0.75, wave + 0.6 - depth_m);
      wc = mix(wc, vec3(0.92, 0.94, 0.9), foam * 0.55);
    }
    if (wake_foam > 0.0) {
      float lace = smoothstep(0.35, 0.75, gnoise(world * 2.2 + vec2(t * 0.3, 0.0)) * 0.5 + 0.5 +
                              (min(wake_foam, 1.0) - 0.5) * 0.8);
      wc = mix(wc, vec3(0.9, 0.93, 0.92) * (0.35 + 0.65 * clamp(sky_sun.w * 2.5, 0.0, 1.0)),
               lace * min(wake_foam, 1.0) * 0.8);
    }
    // the game's effects drawn over the water (foam, splashes, wakes) show through where they're
    // clearly brighter than our water; on a disc lying on the water, anything brighter does
    const vec3 luma = vec3(0.299, 0.587, 0.114);
    float over = dot(c, luma) - dot(wc, luma);
    c = water_decal ? max(wc, c) : mix(wc, c, smoothstep(0.06, 0.2, over));
  }

  // cutscenes: a real depth of field around what the camera looks at (autofocus on the middle of
  // the screen): each sample counts if its own blur reaches this pixel, and something behind only
  // as far as this pixel's blur, so a sharp face doesn't smear over the blurred background
  if (cine_dof.z > 0.0 && wsurf == 0.0) {
    float fz = cine_dof.x;
    float maxr = cine_dof.z;
    float zc = z > 0.0 ? z : 1e9;
    float coc_c = clamp(abs(1.0 - fz / zc) * 1.2 * cine_dof.y, 0.0, 1.0) * maxr;
    vec3 sum = c;
    float wsum = 1.0;
    float rot = pixel_angle();
    for (int i = 0; i < 24; i++) {
      vec2 o = vogel(i, 24, rot) * maxr;
      float rr = length(o);
      vec2 suv = uv + o * texel;
      float sd = texture(tex_T3, suv).r;
      float sz = sd > 1e-7 ? view_z(sd) : 1e9;
      float coc_s = clamp(abs(1.0 - fz / sz) * 1.2 * cine_dof.y, 0.0, 1.0) * maxr;
      float reach = sz < zc ? coc_s : min(coc_s, coc_c);
      float wgt = clamp(reach - rr + 1.0, 0.0, 1.0);
      sum += texture(tex_T0, suv).rgb * wgt;
      wsum += wgt;
    }
    c = sum / wsum;
    blur = max(blur, coc_c / maxr);
  }

  // light depth of field on the far distance (samples nearer than the pixel are skipped)
  // (not on water: its pixels would blur back in the game's own, two-colored water)
  if (dof.z > 0.0 && cine_dof.z == 0.0 && z > 0.0 && wsurf == 0.0) {
    float coc = smoothstep(dof.x, dof.y, z) * dof.z;
    if (coc > 0.25) {
      const vec2 disk[12] = vec2[](vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457),
                                   vec2(-0.203, 0.621), vec2(0.962, -0.195), vec2(0.473, -0.480),
                                   vec2(0.519, 0.767), vec2(0.185, -0.893), vec2(0.507, 0.064),
                                   vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));
      vec3 sum = c;
      float wsum = 1.0;
      for (int i = 0; i < 12; i++) {
        vec2 suv = uv + disk[i] * coc * texel;
        float sd = texture(tex_T3, suv).r;
        float sz = sd > 1e-7 ? view_z(sd) : 1e12;
        float w = sz > z * 0.85 ? 1.0 : 0.0;
        sum += texture(tex_T0, suv).rgb * w;
        wsum += w;
      }
      c = sum / wsum;
      blur = max(blur, coc / dof.z);
    }
  }

  // contrast-adaptive sharpen (AMD CAS, simplified), not where we just blurred
  float sharp = wsurf > 0.0 ? 0.0 : sharpen * (1.0 - blur);  // not on water (raw neighbors)
  if (sharp > 0.0) {
    vec3 n = texture(tex_T0, uv + vec2(0, -texel.y)).rgb;
    vec3 s = texture(tex_T0, uv + vec2(0, texel.y)).rgb;
    vec3 w = texture(tex_T0, uv + vec2(-texel.x, 0)).rgb;
    vec3 e = texture(tex_T0, uv + vec2(texel.x, 0)).rgb;
    vec3 mn = min(c, min(min(n, s), min(w, e)));
    vec3 mx = max(c, max(max(n, s), max(w, e)));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
    vec3 wgt = -amp * mix(0.125, 0.2, sharp) * sharp;
    c = clamp((c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0);
  }

  // ambient occlusion and sun shadows
  float ao = wsurf > 0.0 ? 1.0 : ao_upsample(tex_coord, z);
  if (ao_debug != 0) {
    // right half: distance (grey: log10 of meters / 4, so 1 km = 0.75), sky in blue
    float lm = z > 0.0 ? log(z / 4096.0) / log(10.0) / 4.0 : -1.0;
    color = tex_coord.x > 0.5 ? (z > 0.0 ? vec4(vec3(lm), 1.0) : vec4(0.0, 0.0, 1.0, 1.0)) : vec4(vec3(ao), 1.0);
    return;
  }
  // the surface normal, once: the shadows, the wet ground, the prints, the lights and the
  // reflections all use it
  vec3 surf_n = z > 0.0 ? surface_normal(tex_coord, p) : vec3(0.0, 0.0, -1.0);
  if (normal_debug != 0 && z > 0.0) {
    // the level's own normals in color, the depth-rebuilt ones in grey
    // left half: the normals used (level's own in color, depth-rebuilt in grey); right half: the
    // depth-rebuilt ones in color, to compare
    vec3 nn = surf_n;
    bool own = texture(tex_T11, tex_coord).a > 0.17;
    if (tex_coord.x > 0.5) {
      vec3 pr = view_pos(tex_coord + vec2(texel.x, 0), texture(tex_T3, tex_coord + vec2(texel.x, 0)).r);
      vec3 pu = view_pos(tex_coord + vec2(0, texel.y), texture(tex_T3, tex_coord + vec2(0, texel.y)).r);
      nn = normalize(cross(pr - p, pu - p));
      nn = dot(nn, -p) < 0.0 ? -nn : nn;
      own = true;
    }
    vec3 shown = nn * 0.5 + 0.5;
    color = vec4(own ? shown : vec3(dot(shown, vec3(0.33))), 1.0);
    return;
  }
  c *= ao;

  // rain: everything darkens a little as it soaks, level ground more (it gets the puddles and
  // reflections further down)
  float wet = 0.0;
  float wet_flat = 0.0;  // level ground, where puddles stay after the rest has dried
  vec3 wet_n = vec3(0.0);
  if (max(weather.w, puddle_level) > 0.0 && z > 0.0 && wsurf == 0.0) {
    wet_n = surf_n;
    // (out to the horizon: far away the ground still reads soaked, only the puddles' detail goes)
    wet_flat = smoothstep(0.5, 0.85, dot(wet_n, up_view));
    wet = weather.w * wet_flat;
    c *= 1.0 - 0.1 * weather.x - 0.38 * wet;
    // what isn't level ground (grass, bushes, rocks, walls): soaked too, darker and deeper in color,
    // with a wet sheen at grazing angles and drops glinting on it up close
    float wet_side = weather.w * (1.0 - wet_flat);
    if (wet_side > 0.01) {
      vec3 wq = (view_to_world * p + cam_pos) / 4096.0;
      // (streaks: noise stretched down the slope, where the rain runs off)
      float runs = smoothstep(0.45, 0.75, vnoise3(wq * vec3(1.6, 0.12, 1.6)));
      c = mix(c, c * c * 1.7, 0.5 * wet_side);
      c *= 1.0 - (0.22 + 0.18 * runs) * wet_side;
      vec3 vd = normalize(p);
      float graze = pow(1.0 - clamp(abs(dot(-vd, wet_n)), 0.0, 1.0), 4.0);
      c += (fog_color * 0.35 + 0.04) * graze * wet_side;
      if (z < 25.0 * 4096.0) {
        vec3 wp = (view_to_world * p + cam_pos) / 4096.0;
        float drop = smoothstep(0.86, 0.97, vnoise3(wp * 38.0)) *
                     (0.5 + 0.5 * sin(weather.z * 3.0 + dot(wp, vec3(17.0, 11.0, 23.0))));
        c += vec3(0.85, 0.9, 1.0) * drop * wet_side * 0.35 *
             (1.0 - smoothstep(10.0 * 4096.0, 25.0 * 4096.0, z));
      }
    }
  }

  // snow lying on the ground: first in patches on level ground, then over everything that faces
  // up as it builds; lit by the light that reached the ground there, bluish in the shade. Not on Jak
  // himself (the prints below are pressed into it)
  if (snow_cover > 0.0 && z > 0.0 && wsurf == 0.0) {
    bool on_jak = player_view.w > 0.0 && length(p - player_view.xyz) < 1.5 * 4096.0 &&
                  dot(p - player_view.xyz, up_view) > 0.25 * 4096.0;
    vec3 sw = (view_to_world * p + cam_pos) / 4096.0;
    float upn = dot(surf_n, up_view);
    float n = vnoise3(vec3(sw.xz * 0.35, 0.0)) * 0.6 + vnoise3(vec3(sw.xz * 1.7, 3.0)) * 0.3 +
              vnoise3(vec3(sw.xz * 7.0, 9.0)) * 0.1;
    float slope = smoothstep(mix(0.95, 0.35, snow_cover), mix(1.0, 0.6, snow_cover), upn);
    float th = 1.0 - snow_cover * 1.15;
    float lie = on_jak ? 0.0 : slope * smoothstep(th, th + 0.2, n);
    if (lie > 0.0) {
      // (only as bright as the ground's own light allows, a bit over it: snow is whiter than sand,
      // not a lamp. Capped below white so it keeps its shading, grain from the patch noise)
      // (half from the daylight alone, so it stays white on dark wood and stone too)
      float day = clamp(sky_sun.w * 2.5, 0.0, 1.0);
      float light = clamp(mix(luma(c) * 1.4, 0.55 * (0.3 + 0.7 * day), 0.4), 0.03, 0.72);
      light *= 0.9 + 0.12 * n;
      c = mix(c, vec3(0.88, 0.92, 1.0) * light, lie * 0.9);
    }
    // the bare ground between the patches is damp from melt: darker, less saturated
    float damp = on_jak ? 0.0 : snow_cover * slope * (1.0 - lie);
    c = mix(c, vec3(luma(c)), 0.35 * damp) * (1.0 - 0.25 * damp);
  }

  // footprints and ruts (left where the game said the ground is soft): pressed into the flat ground
  // around them, not into grass, darker inside with a lighter rim of pushed-up sand, fading over
  // half a minute
  if (print_count > 0 && z > 0.0 && z < 40.0 * 4096.0 && wsurf == 0.0) {
    float sandy = (1.0 - smoothstep(1.05, 1.2, c.g / max(c.r, 0.01))) *
                  smoothstep(0.08, 0.2, dot(c, vec3(0.333)));
    vec3 nn = surf_n;
    float level_ground = smoothstep(0.75, 0.9, dot(nn, up_view));
    if (sandy * level_ground > 0.0) {
      vec3 w = view_to_world * p + cam_pos;
      float dent = 0.0, rim = 0.0;
      for (int i = 0; i < print_count; i++) {
        vec3 d = (w - prints[i].xyz) / 4096.0;  // meters
        float kind = print_info[i].y;
        // feet: the ground is at the print; wheels: under the seat
        if (d.y > 0.4 || d.y < (kind > 1.5 ? -2.5 : -0.4) || dot(d.xz, d.xz) > (kind > 1.5 ? 25.0 : 9.0)) {
          continue;
        }
        vec2 f = vec2(sin(prints[i].w), cos(prints[i].w));
        float along = dot(d.xz, f);
        float side = dot(d.xz, vec2(f.y, -f.x));
        float fade = 1.0 - smoothstep(12.0, 30.0, print_info[i].x);
        if (kind > 1.5) {
          // two tracks 1.7 m apart, each piece reaching back to the one before (2 + its length):
          // one solid track, no dashes (a little overlap at both ends hides the joins)
          float seg = max(kind - 2.0, 0.3);
          float t = abs(abs(side) - 0.85);
          float len = smoothstep(-seg - 0.15, -seg + 0.05, along) * (1.0 - smoothstep(0.0, 0.15, along));
          dent = max(dent, (1.0 - smoothstep(0.07, 0.17, t)) * len * fade * 0.8);
          rim = max(rim, smoothstep(0.12, 0.18, t) * (1.0 - smoothstep(0.18, 0.28, t)) * len * fade * 0.5);
        } else {
          // a sole: heel behind, wider toes in front
          float width = mix(0.06, 0.085, smoothstep(-0.1, 0.1, along));
          float e = (along / 0.16) * (along / 0.16) + (side / width) * (side / width);
          dent = max(dent, (1.0 - smoothstep(0.55, 1.0, e)) * fade);
          rim = max(rim, smoothstep(0.9, 1.15, e) * (1.0 - smoothstep(1.15, 1.7, e)) * fade);
        }
      }
      float k = sandy * level_ground * prints_strength;
      // bright sand is over 1 here: brought down to 1 inside the print first, or darkening it
      // still leaves it white
      c = mix(c, min(c, vec3(1.0)), clamp(dent + rim, 0.0, 1.0) * k);
      c *= 1.0 - k * (0.35 * dent - 0.06 * rim * (1.0 - dent));
    }
  }

  // sun shadow map: surfaces facing the sun but blocked from it lose some light (the game's light is
  // baked per vertex and knows nothing of what stands between them and the sun)
  if (shadow_strength > 0.0 && sun_dir.w > 0.0 && z > 0.0 && wsurf == 0.0) {
    vec3 nn = surf_n;
    float ndl = dot(nn, sun_dir.xyz);
    if (ndl > 0.0) {
      // pushed off the surface along its normal (more far away, where a texel is bigger)
      vec3 w = view_to_world * (p + nn * (0.08 * 4096.0 + z * 0.002)) + cam_pos;
      float sh = sun_shadow(w) * smoothstep(0.0, 0.3, ndl);
      if (close_up > 0.0) {
        sh *= smoothstep(0.5 * close_up, close_up, z);
      }
      // the shade is lit by the blue sky only: cooler as well as darker; the sunlit side warmer
      // and fuller (light and shadow apart)
      c *= vec3(1.0) - shadow_strength * sh * vec3(1.05, 0.97, 0.8);
      c *= vec3(1.0) + (1.0 - sh) * smoothstep(0.0, 0.6, ndl) * sun_dir.w * (1.0 - night_dark.x) *
                       vec3(0.08, 0.07, 0.05);
    }
  }

  // cloud shadows: each point looks up toward the sun through the same cloud field the sky draws;
  // where a cloud is in the way it's in shade. The patches drift with the clouds over the ground
  // (fainter when the sky is all cloud: then it's all shade, and the game's light is already dull)
  if (cloud_shadows > 0.0 && clouds.x > 0.0 && clouds.w < 0.6 && sky_sun.w > 0.0 && z > 0.0 &&
      wsurf == 0.0) {
    vec3 sw = world_dir(sky_sun.xyz);
    if (sw.y > 0.06) {
      vec3 wp = (view_to_world * p + cam_pos) / 4096.0;
      float lo = 0.6 - clouds.y * 0.42;
      // (one look through the middle of the layer, counted for three: the patches are big and soft,
      // three looks cost a millisecond)
      float d = 3.0 * cloud_density(wp + sw * ((1450.0 - wp.y) / sw.y), lo, 2);
      float cs = (1.0 - exp(-d * 2.5)) * min(clouds.x, 1.0) * (1.0 - clouds.w) *
                 smoothstep(0.06, 0.2, sw.y) * (1.0 - smoothstep(0.75, 0.97, clouds.y));
      // (faint in the late afternoon: the low sun's light across the valley stays whole)
      c *= vec3(1.0) - 0.5 * (1.0 - 0.75 * golden) * cs * vec3(1.03, 0.97, 0.86);
    }
  }

  // dark nights: where no light reaches the world goes dark (the moon's blue left in it); what
  // shines by itself (windows, fires, lamps: the brightest pixels) stays. The lights below then
  // light the surfaces in their own colors.
  // Around Jak a little is spared (he stays readable, as if the eyes were on him), and the eyes
  // adapt: a dark view is brought up some (from a few points of the scene), a view with lights in
  // it isn't, so they keep their contrast.
  vec3 albedo = c;
  float exposure = 1.0;
  if (night_dark.x > 0.0) {
    float dk = night_dark.x;
    if (player_view.w > 0.0 && z > 0.0) {
      float pd = length(p - player_view.xyz) / 4096.0;
      dk *= 1.0 - 0.25 * (1.0 - smoothstep(0.5, 8.0, pd));
    }
    float l = luma(c);
    float keep = smoothstep(0.4, 0.85, l);
    vec3 moon = vec3(l) * mix(vec3(0.7, 0.82, 1.1), vec3(1.1, 0.55, 0.45), sky_kind.z);
    vec3 darkened = mix(c, moon, 0.45 * dk) * (1.0 - 0.9 * dk);
    c = mix(darkened, c, keep);
    float seen = 0.0;
    for (int j = 0; j < 4; j++) {
      for (int i = 0; i < 5; i++) {
        float sl = luma(texture(tex_T0, vec2((float(i) + 0.5) / 5.0, (float(j) + 0.5) / 4.0)).rgb);
        seen += mix(sl * (1.0 - 0.9 * night_dark.x), sl, smoothstep(0.4, 0.85, sl));
      }
    }
    seen /= 20.0;
    exposure = clamp(0.05 / max(seen, 1e-3), 1.0, 1.0 + 1.2 * night_dark.x);
  }

  // light spill: at night what shines by itself (windows, signs, fires, the lamps' bulbs) lights
  // the surfaces near it on screen, in their own color: the bloom (the bright pixels spread wide)
  // taken as the light falling there. Not on what shines itself.
  if (light_spill > 0.0 && night_dark.x > 0.0 && z > 0.0 && wsurf == 0.0) {
    vec3 spill = texture(tex_T1, tex_coord).rgb;
    float self_lit = smoothstep(0.4, 0.85, luma(albedo));
    c += albedo * min(spill, vec3(0.5)) * 1.3 * light_spill * night_dark.x * (1.0 - self_lit);
  }

  // dynamic lights: each glow lights the surfaces around it that face it, tinted by the surface's
  // own color (the scene's color stands in for its albedo), with a smooth falloff to its radius
  if ((light_count > 0 || spot_count > 0) && z > 0.0 && wsurf == 0.0) {
    vec3 nn = surf_n;
    vec3 added = vec3(0.0);
    // per-pixel jitter of the shadow steps (the TAA averages it)
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) +
                         haze.w * 7.13);
    vec3 from = p + nn * (0.05 * 4096.0 + z * 0.003);
    // headlights: a cone of warm white down the road out to 100 m, and a dim spill around it
    for (int i = 0; i < spot_count; i++) {
      vec3 tl = spot_pos[i].xyz - p;
      float d = length(tl);
      vec3 l = tl / max(d, 1.0);
      float facing = dot(-l, spot_dir[i].xyz);
      float dm = d / 4096.0;
      float ndl = clamp(dot(nn, l) * 0.85 + 0.15, 0.0, 1.0);
      float cone = smoothstep(0.74, 0.95, facing) + 0.12 * smoothstep(0.4, 0.8, facing);
      // gentler in the first meters (the ground under the lamps would burn white)
      float fall = 1.0 / (1.0 + dm * dm / 700.0) * (1.0 - smoothstep(65.0, 100.0, dm)) *
                   (0.4 + 0.6 * smoothstep(2.0, 12.0, dm));
      float lit = cone * fall * ndl * spot_pos[i].w;
      // shadows: rocks, walls and people block the beam (looking up to 12 m back toward the lamp,
      // stopping 3 m short of it: the vehicle itself is in the depth buffer too)
      if (light_shadow > 0.0 && lit > 0.01 && dm > 4.0) {
        float reach = min(d - 3.0 * 4096.0, 12.0 * 4096.0);
        lit *= 1.0 - light_shadow * light_blocked_by(from, spot_pos[i].xyz, jitter, reach, 10);
      }
      added += vec3(1.0, 0.9, 0.72) * lit * 2.6;
    }
    vec3 glints = vec3(0.0);  // the lights' highlights on wet ground
    bool full_day = night_dark.x <= 0.0 && sky_sun.w > 0.05 && clouds.w < 0.3 && weather.x <= 0.0;
    vec3 vdir = normalize(-p);
    for (int i = 0; i < light_count; i++) {
      vec3 to_light = light_pos[i].xyz - p;
      float d = length(to_light);
      float r = light_pos[i].w;
      if (d < r) {
        float k = 1.0 - (d * d) / (r * r);
        float ndl = dot(nn, to_light / max(d, 1.0));
        float lit = clamp(ndl * 0.85 + 0.15, 0.0, 1.0);
        // the strongest lights (sorted first) cast shadows where they still matter
        // (not a light Jak carries: alpha 1, his own shadow from it is a blur across the screen)
        // (not in full daylight: a torch's shadow doesn't show under the sun)
        if (light_shadow > 0.0 && i < 8 && k * k * lit > 0.02 && light_color[i].a < 0.5 && !full_day) {
          lit *= 1.0 - light_shadow * light_blocked(from, light_pos[i].xyz, jitter);
        }
        added += light_color[i].rgb * (k * k) * lit;
        if (wet > 0.0) {
          vec3 hv = normalize(to_light / max(d, 1.0) + vdir);
          glints += light_color[i].rgb * k * pow(max(dot(nn, hv), 0.0), 70.0) * lit;
        }
      }
    }
    // many lamps together (a factory's red floods) saturate softly instead of adding up without end
    added /= 1.0 + 0.35 * max(added.r, max(added.g, added.b));
    if (night_dark.x > 0.0) {
      // at night the light shows the surface's own color, brighter
      // (the game tints the night blue: the surface's own color, warmer, under a lamp)
      vec3 own = albedo * mix(vec3(1.0), vec3(1.3, 1.05, 0.72), night_dark.x);
      c += (own + 0.12) * added * ao * night_dark.y;
    } else {
      c += (c + 0.12) * added * ao;
    }
    c += glints * wet * 1.6 * (1.0 + night_dark.x);
  }

  // indirect light: what the surroundings send, tinted by this surface's own color
  if (gi_strength > 0.0 && wsurf == 0.0) {
    vec3 gi = texture(tex_T7, tex_coord).rgb;
    c += gi * gi_strength * (0.35 + c) * ao * (1.0 - 0.8 * night_dark.x);
  }
  c *= exposure;

  // materials (from the texture names, in the normals' alpha): metal reflects its surroundings
  // tinted by its own color with a broad highlight; glass, marble and tiles reflect them untinted
  // at grazing angles with a tight highlight
  if (materials < 0.0 && z > 0.0) {
    float ga = texture(tex_T11, tex_coord).a;
    int mat = ga > 0.83 ? 0 : (ga > 0.5 ? 1 : (ga > 0.17 ? 2 : 0));
    c = mat == 1 ? vec3(1.0, 0.1, 0.1) : (mat == 2 ? vec3(0.1, 0.3, 1.0) : c * 0.4);
  }
  float mk = materials > 1.5 ? (tex_coord.x < 0.5 ? 1.0 : 0.0) : materials;
  if (mk > 0.0 && z > 0.0 && z < 120.0 * 4096.0 && wsurf == 0.0) {
    float ga = texture(tex_T11, tex_coord).a;
    int mat = ga > 0.83 ? 0 : (ga > 0.5 ? 1 : (ga > 0.17 ? 2 : 0));
    if (mat != 0) {
      vec3 nn = surf_n;
      vec3 vdir = normalize(p);
      vec3 r = reflect(vdir, nn);
      float facing = clamp(dot(-vdir, nn), 0.0, 1.0);
      float hit;
      vec3 refl = trace_reflection(p, r, z, hit);
      float fade = 1.0 - smoothstep(60.0 * 4096.0, 120.0 * 4096.0, z);
      // the sun's highlight, where the sun reaches
      float sunlit = 0.0;
      if (sun_dir.w > 0.0 && dot(nn, sun_dir.xyz) > 0.0) {
        sunlit = 1.0;
        if (shadow_strength > 0.0) {
          sunlit -= sun_shadow(view_to_world * (p + nn * (0.08 * 4096.0 + z * 0.002)) + cam_pos);
        }
        sunlit *= ao;
      }
      float sr = max(dot(r, sun_dir.xyz), 0.0);
      float lum = max(dot(c, vec3(0.3, 0.59, 0.11)), 0.02);
      if (mat == 1) {
        vec3 tint = clamp(c / lum, 0.0, 2.0) * 0.5 + 0.5;
        float f = 0.5 + 0.5 * pow(1.0 - facing, 5.0);
        c = mix(c, refl * tint * (0.5 + lum), f * 0.55 * mk * fade);
        c += c * tint * (pow(sr, 64.0) * 2.0 + pow(sr, 12.0) * 0.45) * sunlit * mk;
      } else {
        float f = 0.04 + 0.96 * pow(1.0 - facing, 5.0);
        c = mix(c, refl, clamp(f * 0.45 + 0.06, 0.0, 1.0) * mk * fade);
        c += vec3(1.0, 0.95, 0.85) * pow(sr, 300.0) * 2.0 * sunlit * mk;
      }
      // the lamps' highlights on it (a factory's pipes, a car's paint at night): metal's broad and
      // tinted by the metal, gloss tight and in the lamp's own color
      for (int i = 0; i < light_count; i++) {
        vec3 tl = light_pos[i].xyz - p;
        float d = length(tl);
        float rr = d / max(light_pos[i].w, 1.0);
        if (rr < 1.6) {
          float lr = max(dot(r, tl / max(d, 1.0)), 0.0);
          float near = 1.0 - smoothstep(0.5, 1.6, rr);
          vec3 tint = mat == 1 ? clamp(c / lum, 0.0, 2.0) * 0.5 + 0.5 : vec3(1.0);
          float sp = mat == 1 ? pow(lr, 40.0) * 0.9 : pow(lr, 150.0) * 1.6;
          c += light_color[i].rgb * tint * sp * near * mk * fade * (1.0 + night_dark.x);
        }
      }
    }
  }

  // wet ground: puddles in the dips of a world-space noise, raindrop rings in them, and the
  // surroundings reflected (strongest in the puddles and at grazing angles: the neon on a wet street)
  if (wet > 0.01 || wet_flat * puddle_level > 0.01) {
    vec3 ww = (view_to_world * p + cam_pos) / 4096.0;
    // wide puddles: low-frequency dips with a ragged rim. A thunderstorm floods them wider; as they
    // dry they draw back into the deepest dips, still water while the street around has dried
    float pd = gnoise(ww.xz * 0.22) + 0.45 * gnoise(ww.xz * 0.7 + 7.1) + 0.12 * gnoise(ww.xz * 3.1 + 2.3);
    float spread = 0.22 * weather2.x - 0.3 * (1.0 - puddle_level);
    float puddle = smoothstep(-0.12 - spread, 0.02 - spread, pd) * smoothstep(0.85, 0.95, dot(wet_n, up_view)) *
                   (1.0 - smoothstep(350.0 * 4096.0, 700.0 * 4096.0, z)) * step(0.001, puddle_level);
    float soaked = wet;
    float water = min(puddle_level * 3.0, 1.0);
    wet = max(wet, puddle * wet_flat * water);
    // (a damp dark rim around a drying puddle)
    float rim = smoothstep(-0.2 - spread, -0.12 - spread, pd) * wet_flat * water * (1.0 - puddle);
    c *= 1.0 - 0.25 * puddle * wet - 0.3 * max(wet - soaked, 0.0) - 0.15 * max(rim - soaked, 0.0);
    vec2 rip = rain_ripples(ww.xz, weather.z) * weather.x * (1.0 - smoothstep(8.0 * 4096.0, 20.0 * 4096.0, z));
    // outside the puddles the wet ground is rough: a fine noise breaks its reflection up
    vec2 rough = vec2(gnoise(ww.xz * 9.0), gnoise(ww.xz * 9.0 + 5.3)) * (1.0 - puddle) *
                 (1.0 - smoothstep(10.0 * 4096.0, 30.0 * 4096.0, z));
    float tw = weather.z;
    vec2 wave = vec2(gnoise(ww.xz * 2.2 + vec2(tw * 0.35, tw * 0.2)),
                     gnoise(ww.xz * 2.2 + vec2(5.0 - tw * 0.25, tw * 0.3))) * 0.07 +
                vec2(gnoise(ww.xz * 7.0 + tw * 0.6), gnoise(ww.xz * 7.0 + 3.3 - tw * 0.5)) * 0.025;
    vec2 bend = rip * (0.15 + 0.6 * puddle) + rough * 0.35 + wave * puddle;
    if (splash_count > 0 && z < 25.0 * 4096.0) {
      bend += step_ripples(ww.xz) * puddle;
    }
    vec3 n2 = normalize(wet_n + right_view * bend.x + fwd_view * bend.y);
    vec3 vdir = normalize(p);
    vec3 r = reflect(vdir, n2);
    float facing = clamp(dot(-vdir, n2), 0.0, 1.0);
    float f = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
    float hit;
    vec3 refl = trace_reflection(p, r, z, hit);
    // a sheen, not a mirror: the ground's own color stays, dimmer reflections outside the puddles
    refl = mix(refl, c, 0.25 * (1.0 - puddle));
    float amount = wet * (0.2 + 0.4 * puddle) * clamp(f * 1.3 + 0.12 * puddle + 0.03, 0.0, 1.0);
    c = mix(c, refl, amount);
    // splashes: drops hitting the ground, each a bright crown spreading for a tenth of a second
    if (weather.x > 0.0 && z < 15.0 * 4096.0) {
      float spl = 0.0;
      for (int k = 0; k < 4; k++) {
        float cs = k == 0 ? 0.3 : 0.45;
        vec2 cell = floor(ww.xz / cs + float(k) * 0.5);
        float h = hash1(cell + float(k) * 13.0);
        float cyc = weather.z * (2.2 + h) + h * 9.0;
        float ph = fract(cyc);
        if (hash1(cell + floor(cyc) * 1.91) < 0.5) {
          vec2 ctr = (cell + 0.2 + 0.6 * vec2(fract(h * 7.3), fract(h * 31.7)) - float(k) * 0.5) * cs;
          float dd = length(ww.xz - ctr);
          float rad = 0.015 + ph * 0.07;
          float life = 1.0 - smoothstep(0.0, 0.12, ph * 0.45);
          spl += exp(-pow((dd - rad) / 0.01, 2.0)) * life + exp(-dd * dd / 0.0002) * life * 0.6;
        }
      }
      float fade = 1.0 - smoothstep(6.0 * 4096.0, 15.0 * 4096.0, z);
      c += (fog_color * 0.5 + 0.22) * spl * fade * weather.x * 0.55;
    }
    // further out: the drops as sparkles about a pixel big, on what faces up, out to 350 m
    if (weather.x > 0.0 && z > 6.0 * 4096.0 && z < 350.0 * 4096.0) {
      float spk = rain_sparkles(ww.xz, weather.z, z / 4096.0 * pixel_rad);
      float fade = smoothstep(6.0 * 4096.0, 15.0 * 4096.0, z) *
                   (1.0 - smoothstep(220.0 * 4096.0, 350.0 * 4096.0, z)) *
                   smoothstep(0.7, 0.9, dot(wet_n, up_view));
      c += (fog_color * 0.5 + 0.22) * min(spk, 1.0) * fade * weather.x * 0.6;
    }
  }

  // relief: the texture's light and dark read as small bumps (bright = raised), lit by the sun:
  // the normal tilts along the brightness slope on the surface; near only, not across edges
  float rk = relief > 1.5 ? (tex_coord.x < 0.5 ? 1.0 : 0.0) : relief;
  if (rk > 0.0 && sun_dir.w > 0.0 && z > 0.0 && z < 30.0 * 4096.0 && wsurf == 0.0) {
    vec2 tx = vec2(texel.x, 0.0), ty = vec2(0.0, texel.y);
    float dr = texture(tex_T3, tex_coord + tx).r, dl = texture(tex_T3, tex_coord - tx).r;
    float du = texture(tex_T3, tex_coord + ty).r, dd = texture(tex_T3, tex_coord - ty).r;
    vec3 pr = view_pos(tex_coord + tx, dr), pl = view_pos(tex_coord - tx, dl);
    vec3 pu = view_pos(tex_coord + ty, du), pd = view_pos(tex_coord - ty, dd);
    float span = 0.004 * z + 0.05 * 4096.0;
    if (abs(pr.z - pl.z) < span && abs(pu.z - pd.z) < span) {
      vec3 nn = surf_n;
      float gx = luma(texture(tex_T0, tex_coord + tx).rgb) - luma(texture(tex_T0, tex_coord - tx).rgb);
      float gy = luma(texture(tex_T0, tex_coord + ty).rgb) - luma(texture(tex_T0, tex_coord - ty).rgb);
      vec3 t = normalize(pr - pl), b = normalize(pu - pd);
      // slope per pixel, a little more where the surface is seen at an angle (its texels stretch)
      vec3 nb = normalize(nn - (t * gx + b * gy) * 1.6);
      float fade = 1.0 - smoothstep(12.0 * 4096.0, 30.0 * 4096.0, z);
      float shade = dot(nb, sun_dir.xyz) - dot(nn, sun_dir.xyz);
      c *= 1.0 + clamp(shade, -0.5, 0.5) * 0.75 * rk * fade * ao;
    }
  }

  // sun relight: the game's light is baked per vertex; shade each pixel a little more by how it
  // faces the sun, with normals rebuilt from the depth buffer (the levels have none)
  if (relight > 0.0 && sun_dir.w > 0.0 && z > 0.0 && wsurf == 0.0) {
    vec3 nn = surf_n;
    float ndl = dot(nn, sun_dir.xyz);
    float k = relight * (1.0 - smoothstep(150.0 * 4096.0, 300.0 * 4096.0, z));
    c *= 1.0 + k * clamp(ndl, -0.6, 1.0) * 0.35;
  }

  // the sky: ours by day over the game's (its painted clouds go), the game's at night; the lightning
  // bolt behind the clouds; the cloud volume over both; greyer under rain
  // (the game's painted cloud cards kilometers out above the horizon count as sky: ours replaces
  // them by day, by night they're smoothed into the game's sky)
  float bolt_light = 0.0;
  float sky_k = z == 0.0 ? 1.0
                         : smoothstep(2800.0 * 4096.0, 4000.0 * 4096.0, z) *
                               smoothstep(0.04, 0.09, world_dir(ray).y) * step(0.0, clouds.x - 0.001);
  if (sky_k > 0.0 && wsurf == 0.0) {
    vec3 c0 = c;
    if (clouds.x > 0.0) {
      vec3 sky = sky_color(ray);
      c = mix(c, sky, clouds.x * (1.0 - clouds.w));
      // the game's own sky by night, dawn and dusk: its painted cloud cards (far geometry, here as
      // sky at any hour) smoothed away: sharp dark cut-outs drifting across it; its colors and its
      // gradient kept, ours are the clouds
      if (clouds.w > 0.0) {
        c = mix(c, game_sky_around(uv, c, vec2(0.3, 0.035)), clouds.x * clouds.w);
      }
      vec4 cl = cloud_info.x > 0.0 ? cloud_fetch(uv) : vec4(0.0, 0.0, 0.0, -1.0);
      if (cl.a < 0.0) {
        cl = cloud_volume(ray, pixel_angle() / 6.2832, mix(sky, c, clouds.w));
      }
      c = mix(c, c * cl.a + cl.rgb, clouds.x);
      if (bolt.w > 0.0 && strike.w == 0.0) {
        bolt_light = d <= 1e-7 ? lightning_bolt(ray, pixel_rad) : 0.0;
      }
    }
    c = mix(c, vec3(luma(c)) * 0.8 * vec3(0.82, 0.94, 1.15), weather.x * 0.3);
    c = mix(c0, c, sky_k);
    bolt_light *= sky_k;
    if (sky_k > 0.5) {
      z = 0.0;  // the weather below treats it as sky
    }
  }
  // a bolt striking the ground: over the sky and whatever is behind it
  if (strike.w > 0.0 && bolt.w > 0.0 && wsurf == 0.0 && (z == 0.0 || z > strike.z * 0.97)) {
    bolt_light = max(bolt_light, lightning_bolt(ray, pixel_rad));
  }
  // the night sky: stars, the moon, a city's glow (over the game's sky)
  if (clouds.w > 0.01 && z == 0.0 && wsurf == 0.0) {
    vec3 base = c;
    c *= 1.0 - 0.75 * night_dark.x;
    c += night_sky(ray, base, pixel_rad);
  }
  // the rainbow, where the sky shows past the rain (or far behind)
  if (weather2.z > 0.0 && sky_sun.w > 0.03 && wsurf == 0.0 && (z == 0.0 || z > 150.0 * 4096.0)) {
    float up = world_dir(ray).y;
    c += rainbow(ray) * weather2.z * 0.13 * smoothstep(-0.02, 0.06, up) *
         clamp(1.0 - sky_sun.w * 1.6, 0.0, 1.0) * (1.0 - clouds.w);
  }

  // fog: thicker with distance and below the camera
  if (fog.z > 0.0 && z > 0.0) {
    float amount = 1.0 - exp(-z * fog.x);
    amount *= clamp(exp(-height * fog.y), 0.0, 1.0);
    vec3 fc = mix(fog_color, vec3(luma(fog_color)) * vec3(0.82, 0.94, 1.15), 0.55 * weather.x);
    fc *= 1.0 - 0.85 * night_dark.x;
    fc += vec3(1.0, 0.55, 0.25) * city_glow * 0.06 * clamp(exp(-height * fog.y), 0.0, 1.0);
    c = mix(c, fc, amount * fog.z);
  }
  // the desert's air: a warm haze lit by the sun, thicker with distance, glowing toward the sun;
  // over the low sky too (the horizon goes golden), not up high
  if (sun_haze > 0.0 && wsurf == 0.0) {
    float dist = z > 0.0 ? length(p) / 4096.0 : 5000.0;
    float up = max(world_dir(ray).y, 0.0);
    float daylight = clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - clouds.w) * (1.0 - weather.x);
    float toward = max(dot(ray, sky_sun.xyz), 0.0);
    // the mountains' shadows in the haze (the god rays pass, alpha), stronger toward the sun
    float lit = 1.0;
    if (haze_shadow > 0.0) {
      // (smoothed over a few of its texels: the march's noise)
      vec2 st = 2.0 / vec2(textureSize(tex_T4, 0));
      float a = texture(tex_T4, uv).a * 0.2 +
                (texture(tex_T4, uv + vec2(st.x, st.y)).a + texture(tex_T4, uv + vec2(-st.x, st.y)).a +
                 texture(tex_T4, uv + vec2(st.x, -st.y)).a + texture(tex_T4, uv + vec2(-st.x, -st.y)).a) * 0.2;
      lit = 1.0 - (1.0 - a) * (0.55 + 0.45 * toward);
    }
    vec3 hc = mix(fog_color, sun_color() * vec3(1.0, 0.76, 0.48), 0.85);
    hc *= (0.9 + (0.5 * toward * toward + 0.6 * pow(toward, 12.0)) * lit) * mix(0.62, 1.0, lit);
    float amount = (1.0 - exp(-dist / 1600.0)) * exp(-up * (z > 0.0 ? 3.0 : 3.5));
    // and dust hanging low over the sand far off: the feet of the mountains and the far dunes
    // fade into it, their tops stand out (height over the camera's, m)
    float rel = z > 0.0 ? world_dir(p).y / 4096.0 : 0.0;
    c = mix(c, hc, clamp(amount * sun_haze * daylight * 0.75, 0.0, 1.0));
    // the late afternoon: the air toward the sun's side of the horizon glows, the far mountains and
    // dunes there melt into a bright gold (by the sun's bearing, not its height: it can stand high)
    if (golden > 0.0) {
      vec2 sh = normalize(world_dir(sky_sun.xyz).xz + 1e-4);
      float az = max(dot(normalize(world_dir(ray).xz + 1e-4), sh), 0.0);
      // (low down: the mountains' feet sink into it, their peaks stand out)
      float hrel = z > 0.0 ? world_dir(p).y / 4096.0 : 0.0;
      float g = pow(az, 3.0) * (1.0 - exp(-dist / 900.0)) * exp(-up * 6.0) * exp(-max(hrel, 0.0) / 90.0) *
                golden * daylight;
      c = mix(c, vec3(1.0, 0.86, 0.6), clamp(g * 0.45, 0.0, 1.0));
    }
    // (the dust in a pale sand color: in the haze's own, under an afternoon sun, it laid a band of
    // bright orange over the middle distance)
    float low = (1.0 - exp(-dist / 450.0)) * exp(-max(rel + 5.0, 0.0) / 18.0) * 0.3;
    vec3 dc = mix(vec3(luma(hc)) * vec3(1.06, 0.98, 0.86), hc, 0.3);
    c = mix(c, dc, clamp(low * sun_haze * daylight * 0.75, 0.0, 1.0));
  }
  // rain mist: the distance goes grey
  if (weather.x > 0.0 && z > 0.0) {
    vec3 grey = vec3(luma(fog_color)) * vec3(0.82, 0.94, 1.15);  // the fog's brightness, cold, not its tint
    grey *= 1.0 - 0.85 * night_dark.x;
    c = mix(c, grey, (1.0 - exp(-z / 4096.0 * (0.007 + 0.006 * weather2.x))) * (0.22 + 0.12 * weather2.x) * weather.x);
  }
  // low fog on the ground: mornings, and steam off the wet ground when the sun comes back out
  float low_fog = weather2.y + 0.1 * weather2.z * (1.0 - clouds.w);
  if (low_fog > 0.0) {
    float dist = z > 0.0 ? length(p) / 4096.0 : 2500.0;
    float daylight = clamp(sky_sun.w * 2.5, 0.0, 1.0) * (1.0 - clouds.w);
    vec3 fc = mix(fog_color, vec3(luma(fog_color)), 0.3) * 0.85 + vec3(0.06, 0.07, 0.08);
    // brighter looking toward the sun
    fc += vec3(1.0, 0.9, 0.75) * pow(max(dot(ray, sky_sun.xyz), 0.0), 6.0) * 0.35 * daylight;
    c = mix(c, fc * (1.0 - 0.85 * night_dark.x), ground_fog(ray, dist, low_fog) * 0.9);
  }
  // dust: the game's sandstorm (its intensity and its wind), as a volume in the world: rolling
  // clouds of sand carried by the wind, streamers low over the ground close by; lit by the sun,
  // brighter looking toward it
  if (weather.y > 0.0) {
    float dust = weather.y;
    float daylight = clamp(sky_sun.w * 2.5, 0.0, 1.0) * (1.0 - clouds.w) + 0.15;
    vec3 sand = mix(vec3(0.8, 0.63, 0.42) * (0.3 + 0.8 * daylight), fog_color, 0.25);
    sand *= 1.0 + pow(max(dot(ray, sky_sun.xyz), 0.0), 4.0) * 0.6 * daylight;
    float dist = z > 0.0 ? length(p) / 4096.0 : 3000.0;
    float cover = dust_cover(ray, dist, dust, pixel_angle() / 6.2832);
    c = mix(c, sand * (1.0 - 0.8 * night_dark.x), cover * 0.94);
  }
  if (puff_count > 0) {
    float daylight = clamp(sky_sun.w * 2.5, 0.0, 1.0) * (1.0 - clouds.w) + 0.12;
    vec3 sand = mix(vec3(0.66, 0.54, 0.4) * (0.2 + 0.65 * daylight), fog_color * 0.9, 0.3);
    vec3 spray = mix(vec3(0.82, 0.87, 0.9) * (0.2 + 0.75 * daylight), fog_color, 0.15);
    float toward = pow(max(dot(ray, sky_sun.xyz), 0.0), 4.0) * 0.35 * daylight;
    float dist = z > 0.0 ? length(p) / 4096.0 : 1e5;
    vec4 rp = ride_puffs(ray, dist, sand * (1.0 + toward), spray * (1.0 + toward),
                         view_to_world * sky_sun.xyz);
    c = c * (1.0 - rp.a) + rp.rgb;
  }

  // volumetric mist: the half-resolution march, upsampled with weights from the depth so the mist
  // behind Jak doesn't bleed over him
  if (volume != 0) {
    vec2 base = (floor(tex_coord / vol_texel - 0.5) + 0.5) * vol_texel;
    vec2 f = (tex_coord - base) / vol_texel;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int j = 0; j < 2; j++) {
      for (int i = 0; i < 2; i++) {
        vec2 tuv = base + vec2(i, j) * vol_texel;
        float tz = view_z(max(texture(tex_T3, tuv).r, texture(tex_T5, tuv).r));  // near sea too
        float bw = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
        float dz = (z > 0.0 && tz > 0.0) ? abs(tz - z) / max(z, 1.0) : (z == tz ? 0.0 : 1.0);
        float w = bw * exp(-dz * 20.0) + 1e-4;
        sum += texture(tex_T8, tuv) * w;
        wsum += w;
      }
    }
    vec4 v = sum / wsum;
    // under rain the mist is grey, whatever the area's fog tint
    v.rgb = mix(v.rgb, vec3(luma(v.rgb)) * vec3(0.82, 0.94, 1.15), weather.x * 0.6);
    c = c * v.a + v.rgb * (1.0 - 0.85 * night_dark.x);
  }

  // fireflies (a green-gold glow that swells and dies every few seconds) or seeds and pollen (pale
  // specks catching the light), hidden behind the scene
  for (int i = 0; i < mote_count; i++) {
    vec3 mp = mote[i].xyz;
    if (mote_style.y <= 0.0 || (z > 0.0 && mp.z > z)) {
      continue;
    }
    vec2 muv = vec2(mp.x * proj.z / mp.z, mp.y * proj.w / mp.z) * 0.5 + 0.5;
    vec2 dpx = (tex_coord - muv) / texel;
    if (dot(dpx, dpx) > 1600.0) {
      continue;
    }
    float seed = mote[i].w;
    if (mote_style.x < 0.5) {
      // a blink: on for about a second out of every 2-5
      float period = 2.0 + 3.0 * fract(seed * 0.618);
      float ph = fract(mote_style.z / period + seed * 0.37);
      float on_k = smoothstep(0.0, 0.1, ph) * (1.0 - smoothstep(0.3, 0.55, ph));
      float radius = clamp(0.05 * 4096.0 * abs(proj.z) * 0.5 / (mp.z * texel.x), 1.3, 4.0);
      float d2 = dot(dpx, dpx);
      float glow = exp(-d2 / (radius * radius)) + 0.35 * exp(-d2 / (radius * radius * 25.0));
      c += vec3(0.75, 1.0, 0.3) * glow * on_k * mote_style.y * 2.6;
    } else {
      float radius = clamp(0.02 * 4096.0 * abs(proj.z) * 0.5 / (mp.z * texel.x), 0.9, 2.2);
      float glow = exp(-dot(dpx, dpx) / (radius * radius));
      // (they twinkle as they turn in the light)
      float tw = 0.6 + 0.4 * sin(mote_style.z * (2.0 + fract(seed) * 3.0) + seed);
      vec3 lit = vec3(1.0, 0.96, 0.86) * (0.35 + 0.9 * clamp(sky_sun.w * 2.5, 0.0, 1.0));
      c = mix(c, lit, clamp(glow * tw * mote_style.y * 1.1, 0.0, 1.0));
    }
  }

  // embers rising from the fires: small sparks drifting up and fading, hidden behind the scene
  for (int i = 0; i < fire_count; i++) {
    vec3 fp = fire_pos[i].xyz;
    float fs = fire_pos[i].w;
    if (fp.z < 1.0 * 4096.0 || fire.y <= 0.0) {
      continue;
    }
    // skip fires far from this pixel on screen (embers rise up to ~5 flame sizes)
    vec2 rel = tex_coord - fire_uv[i];
    if (dot(rel, rel) > dot(fire_axis[i], fire_axis[i]) * 36.0) {
      continue;
    }
    for (int j = 0; j < 24; j++) {
      float s = fire_seed[i] * 17.0 + float(j) * 7.31;
      float h1 = fract(sin(s * 12.9898) * 43758.5453);
      float h2 = fract(sin(s * 78.233) * 43758.5453);
      float h3 = fract(sin(s * 37.719) * 43758.5453);
      float h4 = fract(sin(s * 93.989) * 43758.5453);
      // the last six: the pops, a spark the wood spits out fast now and then
      bool pop = j >= 18;
      float life = pop ? 0.5 + h1 * 0.4 : 1.4 + h1 * 1.6;  // seconds
      float phase = fract(fire.z / life + h2);
      // a new ember each cycle starts somewhere else in the flame
      float cycle = floor(fire.z / life + h2);
      float k1 = fract(sin((s + cycle) * 41.17) * 43758.5453) - 0.5;
      float k2 = fract(sin((s + cycle) * 19.31) * 43758.5453) - 0.5;
      // (a pop only some cycles, flying out sideways too and slowing as it climbs)
      if (pop && fract(sin((s + cycle) * 7.77) * 43758.5453) > 0.35) {
        continue;
      }
      float rise = pop ? (1.0 - (1.0 - phase) * (1.0 - phase)) * fs * (4.0 + 3.0 * h3)
                       : phase * fs * (2.0 + 3.0 * h3);
      float sway = sin(fire.z * (1.5 + h4 * 2.0) + h1 * 6.283) * fs * 0.3 * phase;
      float out_k = pop ? 1.0 + 3.0 * phase : 1.0;
      vec3 ep = fp + up_view * (rise + fs * 0.2) + right_view * (k1 * fs * 0.9 * out_k + sway) +
                fwd_view * (k2 * fs * 0.9 * out_k);
      if (ep.z < 0.3 * 4096.0 || (z > 0.0 && ep.z > z)) {
        continue;
      }
      vec2 euv = vec2(ep.x * proj.z / ep.z, ep.y * proj.w / ep.z) * 0.5 + 0.5;
      vec2 dpx = (tex_coord - euv) / texel;
      float radius = clamp(0.07 * 4096.0 * abs(proj.z) * 0.5 / (ep.z * texel.x), 1.0, 3.5);
      float glow = exp(-dot(dpx, dpx) / (radius * radius));
      if (glow < 0.01) {
        continue;
      }
      float fade = (1.0 - phase) * smoothstep(0.0, 0.08, phase);
      float flick = 0.75 + 0.25 * sin(fire.z * (20.0 + h3 * 15.0) + h4 * 50.0);
      vec3 hot = mix(vec3(1.0, 0.32, 0.04), vec3(1.0, 0.7, 0.25), h4);
      if (pop) {
        hot = mix(vec3(1.0, 0.85, 0.5), hot, phase);  // white-hot as it leaves the flame
      }
      c += hot * (glow * fade * flick * fire.y * (pop ? 3.2 : 2.4));
    }
  }

  // sparks thrown by the explosions: each flies out on its own arc (gravity, a little drag), a
  // short streak along its motion, white-hot to orange as it cools, hidden behind the scene
  for (int i = 0; i < blast_count; i++) {
    float age = blast_age[i];
    if (blast_sparks <= 0.0 || age > 1.5) {
      continue;
    }
    vec3 bp = blast_pos[i].xyz;
    if (bp.z < 1.0 * 4096.0) {
      continue;
    }
    // skip blasts far from this pixel on screen (sparks fly up to ~20 m)
    vec2 cuv = vec2(bp.x * proj.z / bp.z, bp.y * proj.w / bp.z) * 0.5 + 0.5;
    vec2 reach = abs(vec2(proj.z, proj.w)) * 0.5 * (22.0 * 4096.0) / max(bp.z - 20.0 * 4096.0, 2.0 * 4096.0);
    vec2 off = abs(tex_coord - cuv);
    if (off.x > reach.x || off.y > reach.y) {
      continue;
    }
    bool gun = blast_dir[i].w > 0.5;
    bool charging = abs(blast_dir[i].w - 2.0) < 0.5;
    bool charged = blast_dir[i].w > 2.5;
    for (int j = 0; j < (charging ? 10 : 40); j++) {
      float s = blast_seed[i] * 13.7 + float(j) * 3.17;
      float h1 = fract(sin(s * 12.9898) * 43758.5453);
      float h2 = fract(sin(s * 78.233) * 43758.5453);
      float h3 = fract(sin(s * 37.719) * 43758.5453);
      float h4 = fract(sin(s * 93.989) * 43758.5453);
      float life = 0.5 + h4 * 0.9;
      // a gun's blast: the first ones crackle around the muzzle (slow, short), the rest fly
      // forward in a wide fan
      bool crackle = gun && j < 14;
      if (gun) {
        life = crackle ? 0.12 + h4 * 0.25 : 0.25 + h4 * 0.4;
      }
      if (charging) {
        life = 0.06 + h4 * 0.14;
      }
      if (age > life) {
        continue;
      }
      // out and mostly up, in world axes
      vec3 dw = normalize(vec3(h1 - 0.5, 0.25 + h2 * 0.75, h3 - 0.5));
      vec3 dir = right_view * dw.x + up_view * dw.y + fwd_view * dw.z;
      float speed = (7.0 + 13.0 * fract(h1 * 7.3)) * 4096.0;
      if (charging) {
        // all round the gun, short and quick (the radius grows with the charge)
        dir = normalize(vec3(h1 - 0.5, h2 - 0.5, h3 - 0.5) + 1e-4);
        speed = (2.0 + 5.0 * h3) * blast_pos[i].w;
      } else if (crackle) {
        dir = normalize(vec3(h1 - 0.5, h2 - 0.4, h3 - 0.5) + blast_dir[i].xyz * 0.6);
        speed = (2.0 + 4.0 * h3) * 4096.0;
      } else if (gun) {
        vec3 side = normalize(cross(blast_dir[i].xyz, up_view) + 1e-6);
        dir = normalize(blast_dir[i].xyz + side * (h1 - 0.5) * (charged ? 4.5 : 1.8) +
                        up_view * (h2 - 0.3) * 0.5);
        speed = (14.0 + 18.0 * h3) * 4096.0;
      }
      float drag = (1.0 - exp(-age * 2.0)) / 2.0;  // distance flown with drag, per unit speed
      vec3 sp = bp + dir * speed * drag - up_view * (0.5 * 9.8 * 4096.0 * age * age * 0.7);
      vec3 vel = dir * speed * exp(-age * 2.0) - up_view * (9.8 * 4096.0 * age * 0.7);
      vec3 tail = sp - vel * 0.03;
      if (sp.z < 0.5 * 4096.0 || tail.z < 0.5 * 4096.0 || (z > 0.0 && sp.z > z)) {
        continue;
      }
      vec2 a = vec2(sp.x * proj.z / sp.z, sp.y * proj.w / sp.z) * 0.5 + 0.5;
      vec2 b = vec2(tail.x * proj.z / tail.z, tail.y * proj.w / tail.z) * 0.5 + 0.5;
      vec2 pa = (tex_coord - a) / texel, ba = (b - a) / texel;
      float hseg = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-4), 0.0, 1.0);
      vec2 dd = pa - ba * hseg;
      float w = clamp(0.04 * 4096.0 * abs(proj.z) * 0.5 / (sp.z * texel.x), 0.7, 2.5);
      float glow = exp(-dot(dd, dd) / (w * w)) * (1.0 - hseg * 0.6);
      if (glow < 0.01) {
        continue;
      }
      float cool = age / life;
      vec3 hot = gun ? mix(vec3(1.0, 0.85, 0.55), vec3(1.0, 0.18, 0.04), cool)
                     : mix(vec3(1.0, 0.92, 0.65), vec3(1.0, 0.35, 0.05), cool);
      if (charged && !crackle) {
        // red in the middle of the spread, fuchsia toward its edges
        hot = mix(hot, mix(vec3(1.0, 0.5, 0.85), vec3(1.0, 0.1, 0.6), cool), smoothstep(0.1, 0.45, abs(h1 - 0.5)));
      }
      c += hot * glow * (1.0 - cool) * 2.2 * blast_sparks;
    }
  }

  // headlight beams in the air: a few steps along the view through their cones
  if (spot_count > 0) {
    float air_b = weather.x * 0.8 + weather2.y + weather.y * 0.6 + weather2.x * 0.3 + clouds.w * 0.3 +
                  0.35 * night_dark.x;
    float tmax = min(z > 0.0 ? length(p) : 1e9, 70.0 * 4096.0);
    float beam = 0.0;
    float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    for (int i = 0; i < spot_count; i++) {
      for (int k = 0; k < 10; k++) {
        float t = tmax * (float(k) + jit) / 10.0;
        vec3 q = ray * t - spot_pos[i].xyz;
        float dq = length(q);
        float cone = smoothstep(0.86, 0.97, dot(q / max(dq, 1.0), spot_dir[i].xyz));
        float dm = dq / 4096.0;
        beam += cone * spot_pos[i].w / (1.0 + dm * dm / 80.0) * (1.0 - smoothstep(35.0, 70.0, dm)) *
                smoothstep(2.0, 12.0, dm) * tmax / 10.0 / 4096.0;
      }
    }
    // (faint near the lamp: the beam shows down the road, not as a ball of light on the car)
    c += vec3(1.0, 0.9, 0.75) * beam * min(air_b, 1.2) * 0.02;
    // dust hanging in the beams: always a little, a lot in the desert's dust, some in the rain
    if (spot_pos[0].w > 0.0) {
      float motes = beam_motes(ray, z, pixel_rad);
      c += vec3(1.0, 0.93, 0.8) * min(motes, 1.5) * (0.5 + 2.0 * weather.y + 0.4 * weather.x) * 2.6;
    }
  }

  // halos: in rain, fog or dust the air around each light glows (scattering), less where the
  // light is behind something
  // (at night the air always holds some haze: the lamps glow in it, and light the air along the
  // view between them and the scene)
  float air = weather.x * 0.8 + weather2.y + weather.y * 0.5 + weather2.x * 0.3 + clouds.w * 0.35;
  if (air > 0.02 && light_count > 0) {
    vec3 halo = vec3(0.0);
    vec3 shine = vec3(0.0);
    float scene_dist = z > 0.0 ? length(p) : 1e12;
    for (int i = 0; i < min(light_count, 32); i++) {
      // the light scattered toward the camera along the ray up to the scene: the integral of
      // 1 / (h^2 + (t - tc)^2), h the light's distance off the ray
      vec3 lp = light_pos[i].xyz;
      float lr = light_pos[i].w;
      float tc = dot(lp, ray);
      float h = sqrt(max(dot(lp, lp) - tc * tc, 1.0));
      // (lamps, not the big area lights: those would fill the whole view)
      if (h > lr * 1.5 || lr > 20.0 * 4096.0) {
        continue;
      }
      float t1 = min(scene_dist, tc + lr * 3.0);
      float t0 = 0.0;
      if (t1 <= t0) {
        continue;
      }
      float hs = max(h, lr * 0.08);
      // (capped: a bright light passing right by the ray, a dark eco blast in the rain, would
      // otherwise flood the view in its color)
      float inte = min((atan((t1 - tc) / hs) - atan((t0 - tc) / hs)) * lr / hs, 6.0);
      shine += light_color[i].rgb * inte * (1.0 - smoothstep(lr * 0.5, lr * 1.5, h)) *
               smoothstep(2.0 * 4096.0, 6.0 * 4096.0, length(lp));
    }
    shine *= min(air, 1.0) * 0.012;
    // all of them together at most a soft glow (scaled down, its hue kept)
    float shine_max = max(shine.r, max(shine.g, shine.b));
    c += shine * min(1.0, 0.3 / max(shine_max, 1e-4));
    for (int i = 0; i < min(light_count, 16); i++) {
      vec3 lp = light_pos[i].xyz;
      float ld = length(lp);
      if (lp.z <= 0.0 || ld < 1.0) {
        continue;
      }
      float cosang = dot(ray, lp / ld);
      float spread = clamp(light_pos[i].w * 0.4 / ld, 0.01, 0.1);
      // (not the lights right by the camera: their glow would fill the screen)
      float g = exp(-(1.0 - cosang) / (spread * spread * 0.5)) *
                smoothstep(4.0 * 4096.0, 10.0 * 4096.0, ld);
      if (g < 0.003) {
        continue;
      }
      // dimmer where the scene is in front of the light, fading in over its radius (no hard edge)
      float hidden = mix(0.05, 1.0, smoothstep(ld - light_pos[i].w * 1.5, ld, scene_dist));
      halo += light_color[i].rgb * g * hidden;
    }
    c += halo * min(air, 1.5) * 0.25 * (0.25 + 0.75 * clouds.w);
  }

  // the drops Jak's steps throw up from the puddles, lit like the rain
  if (splash_count > 0) {
    float drops = step_drops(tex_coord, z);
    if (drops > 0.0) {
      vec3 glow = texture(tex_T1, tex_coord).rgb;
      vec3 lit = c * 1.15 + vec3(0.08, 0.09, 0.1) + mix(fog_color, vec3(luma(fog_color)), 0.5) * 0.25 +
                 glow * 1.2;
      c = mix(c, lit, drops * 0.45);
    }
  }

  // rain streaks in front of everything, lit by the sky and by the lights around (the bloom)
  if (weather.x > 0.0) {
    float streaks = rain_drops(ray, z, pixel_rad) * (1.0 + 0.9 * weather2.x);
    if (streaks > 0.0) {
      vec3 glow = texture(tex_T1, tex_coord).rgb;
      vec3 lit = c * 1.25 + vec3(0.07, 0.08, 0.1) + mix(fog_color, vec3(luma(fog_color)), 0.5) * 0.25 +
                 glow * 1.6;
      float show = 1.0 - 0.65 * smoothstep(0.45, 0.9, luma(c));
      c = mix(c, lit, clamp(streaks * 0.32 * weather.x * show, 0.0, 0.45));
    }
  }

  // snowflakes in front of everything, white, a little dimmer in dim light
  if (snow_heat.x > 0.0) {
    float flakes = snow_flakes(ray, z, pixel_rad);
    if (flakes > 0.0) {
      vec3 white = vec3(0.92, 0.95, 1.0) * (0.55 + 0.45 * smoothstep(0.05, 0.5, luma(fog_color))) +
                   texture(tex_T1, tex_coord).rgb;
      c = mix(c, white, clamp(flakes * snow_heat.x, 0.0, 0.9));
    }
  }

  // bloom and god rays
  c += texture(tex_T1, tex_coord).rgb * bloom_strength;
  // (god rays a quarter softer than the setting reads: they washed the low sun's side out)
  // (softened over a few of its own texels: it is drawn small and shimmered when stretched)
  {
    vec2 st = 1.5 / vec2(textureSize(tex_T4, 0));
    vec3 shafts = texture(tex_T4, tex_coord).rgb * 0.4 +
                  (texture(tex_T4, tex_coord + vec2(st.x, st.y)).rgb +
                   texture(tex_T4, tex_coord + vec2(-st.x, st.y)).rgb +
                   texture(tex_T4, tex_coord + vec2(st.x, -st.y)).rgb +
                   texture(tex_T4, tex_coord + vec2(-st.x, -st.y)).rgb) * 0.15;
    c += shafts * shafts_strength * 0.75;
  }

  // a town's glow over the horizon at night: warm, low, spread over the town's width, on the sky
  // and the far haze
  if (town_glow.z > 0.0) {
    vec3 wd = normalize(view_to_world * ray);
    vec2 to = town_glow.xy - cam_pos.xz;
    float dist = length(to);
    vec2 hd = normalize(wd.xz + vec2(1e-5));
    float ang = acos(clamp(dot(hd, to / dist), -1.0, 1.0));
    // the town's half width seen from here, at least a few degrees
    float half_w = max(atan(300.0 * 4096.0 / dist), 0.06);
    float across = exp(-pow(ang / (half_w * 1.6), 2.0));
    float elev = wd.y;
    float above = exp(-max(elev + 0.01, 0.0) / 0.16) * smoothstep(-0.06, 0.0, elev);
    float far_w = d <= 1e-7 ? 1.0 : smoothstep(400.0 * 4096.0, 1500.0 * 4096.0, z) * 0.6;
    c += vec3(1.0, 0.55, 0.25) * 0.5 * across * above * far_w * town_glow.z;
  }

  // far lights at night: each far glow a point of light with a soft halo that keeps a few pixels
  // however far it is (the game's own glow shrinks to nothing), hidden behind what's in front
  float aspect_px = texel.y / texel.x;
  float night_air = weather.x * 0.8 + weather2.y + weather.y * 0.5 + weather2.x * 0.3;
  for (int i = 0; i < beacon_count; i++) {
    vec3 bp = beacon_pos[i].xyz;
    vec2 buv = 0.5 * vec2(proj.z * bp.x, proj.w * bp.y) / bp.z + 0.5;
    vec2 dpx = (tex_coord - buv) / texel.y;
    dpx.x *= aspect_px;
    float r2 = dot(dpx, dpx);
    if (r2 < 40.0 * 40.0 && all(greaterThan(buv, vec2(0.0))) && all(lessThan(buv, vec2(1.0)))) {
      float bd = texture(tex_T3, buv).r;
      float front = bd <= 1e-7 ? 1e12 : view_z(bd);
      if (front > bp.z * 0.97 - 2.0 * 4096.0) {
        float dist_m = length(bp) / 4096.0;
        // the glow's own size on screen, at least a couple of pixels
        float core_px = max(1.6, beacon_pos[i].w * 0.35 * abs(proj.w) * 0.5 / bp.z / texel.y);
        float core = exp(-r2 / (core_px * core_px));
        float halo = exp(-sqrt(r2) / (5.0 + 6.0 * min(night_air, 1.0))) * 0.18;
        float fade = exp(-dist_m / 1800.0);
        c += beacon_color[i].rgb * (core * 1.4 + halo) * beacon_color[i].w * fade;
      }
    }
  }
  // Jak's lantern bulb: a warm point hovering by him in a thin metal rim, its glow in the night air
  // around it (the air glows even where something hides the bulb)
  if (lantern.w > 0.0 && lantern.z > 0.5 * 4096.0) {
    vec3 lp = lantern.xyz;
    vec2 luv = 0.5 * vec2(proj.z * lp.x, proj.w * lp.y) / lp.z + 0.5;
    vec2 dpx = (tex_coord - luv) / texel.y;
    dpx.x *= aspect_px;
    float r = length(dpx);
    float core_px = max(2.0, 0.08 * 4096.0 * abs(proj.w) * 0.5 / lp.z / texel.y);
    float ld = texture(tex_T3, clamp(luv, vec2(0.0), vec2(1.0))).r;
    float vis = (ld <= 1e-7 || view_z(ld) > lp.z - 0.3 * 4096.0) ? 1.0 : 0.0;
    float bulb = exp(-r * r / (core_px * core_px)) * 2.2 * vis;
    // its casing: a thin dark band with a bright edge where the bulb's light catches the metal
    float band = exp(-pow((r - core_px * 1.7) / (core_px * 0.35), 2.0)) * vis;
    float edge = exp(-pow((r - core_px * 2.05) / (core_px * 0.18), 2.0)) * vis *
                 (0.6 + 0.4 * dpx.y / max(r, 1e-3));
    float halo = exp(-r / (core_px * 4.0)) * 0.6 + exp(-r / (core_px * 16.0)) * 0.2;
    c *= 1.0 - 0.55 * band * lantern.w;
    // white-yellow in its heart, the glow deep orange
    c += (vec3(1.0, 0.82, 0.5) * bulb + vec3(1.0, 0.7, 0.4) * edge * 0.5 +
          vec3(1.0, 0.5, 0.18) * halo * (0.4 + 0.6 * vis)) * lantern.w;
  }

  // the headlamps themselves seen from in front: a blinding point and a short streak
  for (int i = 0; i < spot_count; i++) {
    vec3 sp = spot_pos[i].xyz;
    if (sp.z > 1.0 * 4096.0) {
      float into = dot(normalize(-sp), spot_dir[i].xyz);
      float glare = smoothstep(0.55, 0.95, into) * spot_pos[i].w;
      if (glare > 0.0) {
        vec2 suv = 0.5 * vec2(proj.z * sp.x, proj.w * sp.y) / sp.z + 0.5;
        vec2 dpx = (tex_coord - suv) / texel.y;
        dpx.x *= aspect_px;
        float bd = texture(tex_T3, suv).r;
        float front = bd <= 1e-7 ? 1e12 : view_z(bd);
        if (front > sp.z * 0.95 - 1.0 * 4096.0) {
          float r = length(dpx);
          float core_px = max(2.0, 0.12 * 4096.0 * abs(proj.w) * 0.5 / sp.z / texel.y);
          float g = exp(-r * r / (core_px * core_px)) * 1.2 + exp(-r / 10.0) * 0.15 +
                    exp(-abs(dpx.y) * 0.8) * exp(-abs(dpx.x) / 60.0) * 0.12;
          c += vec3(1.0, 0.94, 0.82) * g * glare;
        }
      }
    }
  }

  // lens flare: looking toward the sun, the lens's glass throws ghosts of it along the line through
  // the middle of the picture, a faint halo ring and a short streak. Only as much as the sun itself
  // shows (the sky around its disk, not hidden behind a wall or the clouds).
  if (lens_flare > 0.0 && sky_sun.z > 0.0 && sky_sun.w > 0.0 && clouds.w < 0.5) {
    vec2 suv = 0.5 * vec2(proj.z * sky_sun.x, proj.w * sky_sun.y) / sky_sun.z + 0.5;
    if (all(greaterThan(suv, vec2(-0.3))) && all(lessThan(suv, vec2(1.3)))) {
      float vis = 0.0;
      for (int i = 0; i < 12; i++) {
        vec2 q = suv + vogel(i, 12, 0.0) * vec2(texel.x / texel.y, 1.0) * 0.012;
        if (all(greaterThan(q, vec2(0.0))) && all(lessThan(q, vec2(1.0))) &&
            texture(tex_T3, q).r <= 1e-7) {
          vis += smoothstep(0.55, 0.95, luma(texture(tex_T0, q).rgb));
        }
      }
      vis /= 12.0;
      if (vis > 0.0) {
        float aspect = texel.y / texel.x;
        vec2 axis = vec2(0.5) - suv;
        vec3 fl = vec3(0.0);
        // ghosts: discs of a few sizes and tints, some behind the middle, one past it
        const float at[5] = float[](0.45, 0.8, 1.25, 1.55, 2.1);
        const float rad[5] = float[](0.025, 0.05, 0.018, 0.08, 0.035);
        const vec3 tint[5] = vec3[](vec3(1.0, 0.75, 0.4), vec3(0.4, 0.8, 0.6), vec3(0.9, 0.5, 0.9),
                                    vec3(0.35, 0.5, 1.0), vec3(1.0, 0.85, 0.5));
        for (int k = 0; k < 5; k++) {
          vec2 gp = suv + axis * at[k];
          float d = length((tex_coord - gp) * vec2(aspect, 1.0));
          fl += tint[k] * (1.0 - smoothstep(rad[k] * 0.5, rad[k], d)) * 0.07;
        }
        // the halo ring around the middle of the picture, toward the sun
        vec2 dv = (tex_coord - vec2(0.5)) * vec2(aspect, 1.0);
        float ring = exp(-pow((length(dv) - 0.42) / 0.025, 2.0)) *
                     max(dot(normalize(dv + 1e-5), normalize(-axis * vec2(aspect, 1.0) + 1e-5)), 0.0);
        fl += vec3(0.9, 0.75, 1.0) * ring * 0.025;
        // a short horizontal streak through the sun
        vec2 ds = (tex_coord - suv) * vec2(aspect, 1.0);
        fl += vec3(1.0, 0.9, 0.75) * exp(-abs(ds.y) * 400.0) * exp(-abs(ds.x) * 6.0) * 0.18;
        c += fl * vis * lens_flare * clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - clouds.w * 2.0) * (1.0 - 0.7 * weather.x);
      }
    }
  }

  // highlights roll off instead of clipping (bloom can push past 1): first the brightness, from
  // 0.7 up (the hue kept: sunlit sand and walls keep their color instead of burning to yellow and
  // white), then each channel
  {
    // a strongly colored light past full (a red lamp on a wall) bleaches toward white as film and
    // eyes see it, instead of flattening into one pure, detail-less color
    float mx = max(c.r, max(c.g, c.b));
    c = mix(c, vec3(mx), clamp((mx - 1.0) * 0.35, 0.0, 0.45));
    float hl = luma(c);
    if (hl > 0.7) {
      float over = hl - 0.7;
      c *= (0.7 + over / (1.0 + over * 2.2)) / hl;
    }
  }
  c = c / (1.0 + max(c - 0.85, 0.0) * 1.2);
  // by day, a white balance: the scene's average color (a few points of it) brought part of the
  // way to grey, so a strong cast (the desert's yellow afternoons) doesn't tint everything
  if (clouds.w < 0.99) {
    vec3 avg = vec3(0.0);
    for (int j = 0; j < 4; j++) {
      for (int i = 0; i < 5; i++) {
        avg += texture(tex_T0, vec2((float(i) + 0.5) / 5.0, (float(j) + 0.5) / 4.0)).rgb;
      }
    }
    avg = max(avg / 20.0, vec3(1e-3));
    vec3 wb = clamp(vec3(luma(avg)) / avg, vec3(0.8), vec3(1.25));
    c *= mix(vec3(1.0), wb, 0.35 * (1.0 - clouds.w));
  }

  // rock and cliffs (normals written 0.75 long: tfrag3.frag): the game's textures are a blur up
  // close and stretched over whole mountains, so a tiling rock picture (rock_detail.png) is laid
  // over them in the world from three sides (triplanar), three sizes, its brightness only (the
  // game's colors kept). Each read at the mip level of the pixel's size (no derivatives in here):
  // too small for the pixels, a size averages out by itself
  if (rock_detail > 0.0 && z > 0.0) {
    float nl = length(texture(tex_T11, tex_coord).rgb * 2.0 - 1.0);
    if (nl > 0.7 && nl < 0.8) {
      vec3 wp = cam_pos / 4096.0 + world_dir(p) / 4096.0;  // m
      vec3 wn = world_dir(surf_n);
      // (sharp: the view from above, its strata lines, stays off steep faces, where they ran down
      // the rock)
      vec3 tw = pow(abs(wn), vec3(8.0));
      tw /= tw.x + tw.y + tw.z;
      float foot = length(p) / 4096.0 * pixel_rad / max(abs(dot(ray, surf_n)), 0.1);  // m per pixel
      float d = 0.0;
      float dist = length(p) / 4096.0;  // m
      // (as big games do it: the picture is close-up detail, each size only within its own reach
      // (the small one near, the big one out to ~150 m); farther the rock keeps only a soft, broad
      // light and dark (macro variation), so big faces far away don't show a repeated pattern that
      // fights their own texture)
      // (and in the late afternoon a third, mountain-sized: the low sun rakes the far faces, their
      // cracks and blocks show out to the mountains)
      const vec4 size[3] = vec4[](vec4(1.6, 0.5, 15.0, 45.0), vec4(7.0, 0.85, 60.0, 160.0), vec4(60.0, 0.8, 900.0, 1600.0));  // (tile m, strength, fade from, to)
      // (the picture: cracked blocky rock; its blocks shift a little from place to place, and it
      // comes and goes: strong in some places, fainter in others)
      float wander = gnoise(wp.xz / 9.0 + vec2(0.0, wp.y / 25.0) + 1.3) * 0.8;
      float bands = 0.5 + 0.5 * smoothstep(-0.35, 0.35, gnoise(vec2(wp.x + wp.z, wp.y) / 30.0 + 5.3));
      for (int k = 0; k < 3; k++) {
        float lod = max(log2(foot * 1024.0 / size[k].x), 0.0);
        float near = 1.0 - smoothstep(size[k].z, size[k].w, dist);
        if (k == 2) {
          near *= golden * smoothstep(60.0, 160.0, dist);
        }
        if (lod > 9.0 || near <= 0.0) {
          continue;  // (one texel over the pixel, or past its reach: nothing to add)
        }
        vec3 q = wp / size[k].x + float(k) * 0.37;
        vec3 qs = q + vec3(wander, 0.0, wander) / size[k].x;
        float t = luma(textureLod(tex_T10, qs.zy, lod).rgb) * tw.x + luma(textureLod(tex_T10, q.xz, lod).rgb) * tw.y +
                  luma(textureLod(tex_T10, qs.xy, lod).rgb) * tw.z;
        d += size[k].y * near * (t - rock_detail_avg);
      }
      // (the broad variation: a few tens of meters, gentle, it fades in as the detail fades out)
      float macro = gnoise(vec2(wp.x + wp.y * 0.6, wp.z - wp.y * 0.4) / 45.0 + 2.1) * 0.6 +
                    gnoise(vec2(wp.x - wp.y * 0.5, wp.z + wp.y * 0.7) / 13.0 + 7.4) * 0.4;
      d += macro * 0.12 * (0.4 + 0.6 * smoothstep(30.0, 150.0, dist));
      c *= max(1.0 + rock_detail * 1.6 * d * bands, 0.3);
    }
  }

  // the desert's sand: the game paints it olive, and lit at dusk it goes green. Its yellow-greens
  // (and the dull dark greens of its pebbles) turned toward warm gold; the sky and the strong
  // greens of plants kept
  // (the sand ripples' meters across the crests per pixel, from the screen's derivatives, out
  // here where every pixel runs: the crests run nearly toward the camera, and along them the
  // ground isn't squeezed by the grazing view, so a wave stays sharp much farther than the
  // pixel's worst-case size says)
  const vec2 rip_across = vec2(0.82, 0.57);  // the crests run across the wind
  float rip_m = fwidth(dot((cam_pos.xz + world_dir(p).xz) / 4096.0, rip_across));
  if (sand > 0.0 && z > 0.0) {
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float ch = mx - mn;
    if (ch > 1e-4) {
      // hue (0..1), as hsv
      float hue = mx == c.r ? mod((c.g - c.b) / ch, 6.0) : mx == c.g ? (c.b - c.r) / ch + 2.0 : (c.r - c.g) / ch + 4.0;
      hue /= 6.0;
      float sat = ch / mx;
      // olive and yellow-green fully; the dull greens of the pebbles less (strong greens stay)
      float w = smoothstep(0.07, 0.12, hue) * (1.0 - smoothstep(0.24, 0.30, hue));
      w = max(w, smoothstep(0.2, 0.26, hue) * (1.0 - smoothstep(0.42, 0.5, hue)) * (1.0 - smoothstep(0.35, 0.6, sat)) * 0.8);
      w *= smoothstep(0.03, 0.12, sat) * sand;
      if (w > 0.0) {
        // (a pale, dusty beige: less saturated than the game's olive (no lighter: noon burned out); never
        // past a sand's saturation: the yellower patches went bright orange, in blotches and bands)
        // (in the late afternoon: amber, and as rich as the low sun makes it)
        float h2 = mix(hue, mix(0.095, 0.075, golden), w);
        float s2 = mix(sat, mix(min(sat, 0.3), clamp(sat * 1.25, 0.4, 0.5), golden), w);
        vec3 k = clamp(abs(mod(h2 * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
        c = mx * mix(vec3(1.0), k, s2);
      }
      // and its pale slopes: the game lights some near white, a little blue (any hue, barely any
      // color, bright): to a sand beige, a touch darker
      float wp = (1.0 - smoothstep(0.08, 0.2, sat)) * smoothstep(0.45, 0.7, mx) * sand * (1.0 - w);
      if (wp > 0.0) {
        vec3 beige = clamp(abs(mod(0.095 * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
        c = mix(c, mx * 0.92 * mix(vec3(1.0), beige, 0.27), wp);
      }
    }
    // on the sand (its normals written 0.9 long: tfrag3.frag, hfrag.frag): the wind's ripples,
    // made up in the world (the game's sand is a blur up close), shaded by the sun across them.
    // By the mark only, not the color: the warm haze turns the far sand orange
    float nl = length(texture(tex_T11, tex_coord).rgb * 2.0 - 1.0);
    float dune = step(0.85, nl) * step(nl, 0.95);
    float level = dot(surf_n, up_view);
    float foot = length(p) / 4096.0 * pixel_rad / max(abs(dot(ray, up_view)), 0.05);  // m per pixel
    float rip = dune * smoothstep(0.4, 0.75, level);
    if (rip > 0.0) {
      vec2 xz = cam_pos.xz / 4096.0 + world_dir(p).xz / 4096.0;
      vec2 across = rip_across;
      vec2 sun_xz = world_dir(sky_sun.xyz).xz;
      float facing = dot(across, sun_xz) / max(length(sun_xz), 1e-3);
      // in patches: sharp in some places, smoothed out in others
      float patches = 0.35 + 0.65 * smoothstep(-0.25, 0.35, gnoise(xz * 0.11 + 3.7));
      // the same ripples at four sizes, each a copy of the smallest scaled up (its wiggles too),
      // each kept while a wave spans 2+ pixels, and their weights renormalized: where the small
      // ones fade the bigger take over at the same strength, so far off looks like up close
      // (the waves' spacing on screen stays about the same, as perspective shrinks them)
      float shade = 0.0, wsum = 0.0;
      const float octave[4] = float[](0.24, 0.9, 3.4, 12.0);  // wavelength, m
      for (int k = 0; k < 4; k++) {
        float wl = octave[k];
        // (while a wave spans ~2 pixels across the crests; the depth's edges give a huge
        // derivative: there the pixel's own size)
        float m = rip_m < foot * 3.0 ? rip_m : foot;
        float vis = 1.0 - smoothstep(wl / 5.0, wl / 1.8, m);
        if (vis > 0.0) {
          float sc = 0.24 / wl;
          vec2 o = vec2(5.1, 3.3) * float(k);
          // wiggles, and a slow bend of many waves over tens of meters: the wind's direction drifts,
          // the crests turn with it (a rotation would slip by the distance from the world's origin)
          float wk = gnoise(xz * 0.35 * sc + o) * 2.2 + gnoise(xz * 1.3 * sc + o.yx) * 0.6 +
                     gnoise(xz * 0.03 * sc + o * 0.3) * 9.0;
          float phase = dot(xz, across) * 6.2832 / wl + wk * 6.2832;
          float crest = sin(phase) + 0.35 * sin(2.0 * phase);
          float slope = cos(phase) + 0.7 * cos(2.0 * phase);
          // crests broken off here and there (noise long along them, short across): ends and gaps
          // instead of endless parallel lines
          vec2 cr = vec2(dot(xz, vec2(-across.y, across.x)) / (wl * 7.0), dot(xz, across) / (wl * 1.8));
          float whole = 0.25 + 0.75 * smoothstep(-0.3, 0.3, gnoise(cr + o * 1.7));
          // (the smallest fully up close, the next ones mixed under it a little)
          float wgt = vis * (k == 0 ? 1.0 : 0.35);
          shade += wgt * whole * (0.08 * slope * facing + 0.025 * crest);
          wsum += wgt;
        }
      }
      shade *= 2.6 * min(wsum / 0.35, 1.0) / max(wsum, 1e-3);  // (the biggest fades out alone)
      // crests across the view (stripes along the horizon) read far too strong in the distance:
      // fainter there, the ones running toward the camera kept
      vec2 look = normalize(world_dir(p).xz + 1e-4);
      float side = abs(dot(look, across));
      float rdist = length(p) / 4096.0;
      shade *= 1.0 - 0.7 * side * side * smoothstep(40.0, 200.0, rdist);
      // and all of them softer far off: the big waves there drew thick dark lines over the dunes
      shade *= 1.0 - 0.65 * smoothstep(50.0, 220.0, rdist);
      float fine = 1.0 - smoothstep(0.006, 0.015, foot);
      float grain = fine > 0.0 ? gnoise(xz * 23.0) * 0.6 + gnoise(xz * 61.0) * 0.4 : 0.0;  // (near only)
      c *= 1.0 + rip * (patches * shade + 0.07 * grain * fine);
    }
  }

  // grading: the area's tint (shadows lifted toward one color, highlights toward another), then
  // saturation, gentle S-curve, warm highlights / cool shadows
  // rain: colder, the colors kept (a touch richer, like the wet slums at night)
  c *= mix(vec3(1.0), vec3(0.9, 0.98, 1.1), weather.x);
  c = max(mix(vec3(luma(c)), c, 1.0 + 0.1 * weather.x), 0.0);
  // the desert under a high sun: the game lights its ground near white, the eye stops down (the
  // ground only: the sky keeps its own)
  if (sun_haze > 0.0 && z > 0.0 && wsurf == 0.0) {
    float high = sun_haze * noon * (1.0 - clouds.w);
    c *= 1.0 - 0.49 * high;
    // (and the color the glare washed out of it back: sand, not chalk)
    c = max(mix(vec3(luma(c)), c, 1.0 + 0.2 * high), 0.0);
  }
  // the late afternoon: amber and deep, the lit parts richer, the shade darker
  if (golden > 0.0 && z > 0.0) {
    float lg = luma(c);
    c = max(mix(vec3(lg), c, 1.0 - 0.15 * golden), 0.0);
    // (the sand stopped down; the mountains lifted, the low sun lights them face on. By the
    // normals' tag lengths, tfrag3.frag / hfrag.frag)
    float tag = length(texture(tex_T11, tex_coord).rgb * 2.0 - 1.0);
    float rock_expo = mix(1.0, 1.12, smoothstep(15.0, 60.0, length(p) / 4096.0));
    float expo = tag > 0.85 && tag < 0.95 ? 0.82 : tag > 0.7 && tag < 0.8 ? rock_expo : 1.0;
    // (the grey rock slabs in the sand: warm sandstone in this light, not ash)
    if (tag > 0.7 && tag < 0.8) {
      float mxr = max(c.r, max(c.g, c.b));
      float satr = (mxr - min(c.r, min(c.g, c.b))) / max(mxr, 1e-3);
      float grey = (1.0 - smoothstep(0.3, 0.6, satr)) * (1.0 - smoothstep(40.0, 120.0, length(p) / 4096.0));
      // (and lit: the game bakes them near black; the low sun catches their tops)
      float lift = mix(1.55, 1.2, smoothstep(20.0, 80.0, length(p) / 4096.0));
      c = mix(c, min(lg * lift, 0.5) * vec3(1.32, 0.95, 0.62), golden * grey * 0.85);
    }
    c *= mix(vec3(1.0), vec3(1.05, 0.99, 0.92) * expo, golden);
    c *= 1.0 - 0.4 * golden * (1.0 - smoothstep(0.2, 0.55, lg));
  }
  c = c * grade_gain + grade_lift * (1.0 - clamp(c, 0.0, 1.0));
  float l = luma(c);
  c = max(mix(vec3(l), c, saturation * (1.0 + 0.12 * weather2.z)), 0.0);
  vec3 curve = c * c * (3.0 - 2.0 * c);
  c = mix(c, curve, contrast);
  // (less in the brightest parts: they'd go yellow)
  c += warmth * vec3(1.0, 0.25, -0.75) * (l - 0.35) * (1.0 - 0.6 * smoothstep(0.55, 0.9, l));

  // the desert under a high sun: the game's ground comes in near white (bluish on the slopes), the
  // grade above whiter still: its highlights rolled off softly (a film shoulder) and warmed back
  // to sand, instead of paper
  if (sun_haze > 0.0 && z > 0.0 && wsurf == 0.0) {
    float ls = luma(c);
    // (only the near whites: one squeezing everything over the mid tones flattened the sand,
    // its ripples and soft shading gone; the exposure above brings it down instead)
    if (ls > 0.7) {
      float to = 0.7 + (ls - 0.7) * 0.6 / (1.0 + (ls - 0.7) * 2.0);
      vec3 tone = c * (to / ls);
      tone = max(mix(vec3(to), tone, 1.35), 0.0);  // (some of the saturation back)
      float k = smoothstep(0.75, 0.95, ls);        // (the whiter, the more toward sand)
      tone = mix(tone, to * vec3(1.2, 0.98, 0.7), 0.7 * k);
      c = mix(c, tone, min(sun_haze * 1.25, 1.0));
    }
  }

  // lightning, after the grade (white, whatever the area's tint): the flash lights everything cold
  // for an instant, the sky most, and the bolt over the sky
  if (weather2.w > 0.0 || bolt_light > 0.0 || sheet > 0.0) {
    vec3 cold = vec3(0.85, 0.9, 1.0);
    c += cold * weather2.w * ((z == 0.0 ? 0.04 : 0.03) + c * 0.45);
    // sheet lightning: the sky glows, the scene below catches it
    c += cold * sheet * ((z == 0.0 ? 0.22 : 0.035) + c * 0.4);
    c += cold * bolt_light * 2.2;
  }

  // vignette
  vec2 v = tex_coord - 0.5;
  c *= 1.0 - vignette * smoothstep(0.25, 0.85, dot(v, v) * 2.2);

  color = vec4(clamp(c, 0.0, 1.0) * color_mult.rgb * color_mult.a, 1.0) + color_add;
}
