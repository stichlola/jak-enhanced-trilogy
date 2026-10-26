#version 430 core

// Jak 3 Enhanced: composite of the 3D scene, drawn back over it before the HUD buckets.
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
uniform float prints_strength;
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
uniform float relight;      // sun relight strength (0 = off)  // how much of our water color replaces the game's

uniform vec4 color_mult;
uniform vec4 color_add;
uniform vec3 grade_lift;        // the area's grade: added in the shadows
uniform vec3 grade_gain;        // and multiplying the highlights
uniform sampler2D tex_T13;      // the sun shadow map as plain depths (soft shadows' blocker search)
uniform vec4 shadow_params;     // depth span (m), map side (m), penumbra per meter, soft (1) or not
uniform vec4 sky_sun;           // toward the sun (or moon) in view space, day or night; w: its height
uniform vec4 clouds;            // amount (0 = off), sky cover, time (s), night (0..1)
uniform vec4 weather;           // rain, dust (0..1), time (s), wet ground (0..1)
uniform vec4 weather2;          // thunderstorm, ground fog, the sun out after the rain, lightning flash
uniform vec4 dust_wind;         // the wind (world xz, unit), how far it carried the dust (m)
uniform float sheet;            // sheet lightning: the whole sky lit from inside the clouds (0..1)
uniform float sunny;            // a bright summer day (Jak 1's Sandover): deep blue sky, white clouds
uniform vec4 bolt;              // lightning bolt: azimuth (rad), seed, age (s), strength
uniform vec3 cine_dof;          // cutscenes: focus (view z, game units), aperture, max blur (px; 0 off)
uniform float blast_sparks;     // explosions' sparks and hot air
uniform float blast_seed[4];

in vec2 tex_coord;
out vec4 color;

float luma(vec3 c) {
  return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// gradient noise, for water that doesn't repeat
vec2 hash2(vec2 q) {
  q = vec2(dot(q, vec2(127.1, 311.7)), dot(q, vec2(269.5, 183.3)));
  return -1.0 + 2.0 * fract(sin(q) * 43758.5453);
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
float light_blocked(vec3 from, vec3 lp, float jitter) {
  vec3 dir = lp - from;
  float len = length(dir);
  float reach = min(len * 0.85, 3.0 * 4096.0);
  vec3 stp = dir / len * (reach / 8.0);
  for (int s = 0; s < 8; s++) {
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
  return mix(env, ssr, ssr_hit);
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
  float top = 0.14 + 0.16 * fract(bolt.y * 0.37);
  float a = bolt.z;
  if (bolt.w <= 0.0 || a > 0.42) {
    return 0.0;
  }
  vec3 wd = world_dir(ray_view);
  float el = wd.y;
  if (el < -0.01 || el > top) {
    return 0.0;
  }
  float da = mod(atan(wd.x, wd.z) - bolt.x + 3.14159, 6.28318) - 3.14159;
  if (abs(da) > 0.5) {
    return 0.0;
  }
  float u = (top - el) / top;
  // the leader's tip
  float lead = clamp(a / 0.09, 0.0, 1.0);
  if (u > lead) {
    return 0.0;
  }
  float w = max(px * 1.3, 0.0012);
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

// the sun's color by its height, the moon's at night
vec3 sun_color() {
  vec3 col = mix(vec3(1.0, 0.5, 0.28), vec3(1.0, 0.96, 0.9), smoothstep(0.05, 0.35, sky_sun.w));
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
  return clamp((n * shape - lo) * 4.0, 0.0, 1.0);
}

vec4 cloud_volume(vec3 dv, float jitter, vec3 sky) {
  vec3 wd = world_dir(dv);
  if (wd.y < 0.015) {
    return vec4(0.0, 0.0, 0.0, 1.0);
  }
  float cam_y = cam_pos.y / 4096.0;
  float t0 = max((1100.0 - cam_y) / wd.y, 0.0);
  float t1 = min((2000.0 - cam_y) / wd.y, t0 + 9000.0);
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
  float near_bolt = exp(-pow(mod(atan(wd.x, wd.z) - bolt.x + 3.14159, 6.28318) - 3.14159, 2.0) / 0.2);
  vec3 flash = vec3(0.8, 0.85, 1.0) * (weather2.w * (0.12 + 1.1 * near_bolt) + sheet * 0.9);
  const int N = 18;
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
      float a = 1.0 - exp(-d * 0.022 * dt);
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
  return vec4(sum, trans);
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

// raindrops in the puddles: each 0.5 m cell gets a ring now and then, spreading and fading
// (returns the slope it gives the water, xz)
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

void main() {
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
    if (wd > d + 1e-7) {
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
    float clear_water = exp(-depth_m / 2.0);

    // the swell: long waves in open water, faded out with distance (they'd shimmer) and depth
    float swell_amt = water_swell_strength * smoothstep(1.0, 6.0, depth_m) *
                      (1.0 - smoothstep(0.3, 1.5, water_foot));
    if (swell_amt > 0.0) {
      float s0 = water_swell(world, t);
      slope += swell_amt * vec2(water_swell(world + vec2(0.5, 0.0), t) - s0,
                                water_swell(world + vec2(0.0, 0.5), t) - s0) / 0.5;
    }
    // wakes: rings spreading from where Jak touched the water lately (each point of his path near
    // this water's height)
    float water_y = dot(up_view, p) + cam_pos.y;
    for (int i = 0; i < wake_count; i++) {
      // (Jak's root is his feet: up to 2.5 m under the surface swimming, over it riding)
      float above = wake[i].y - water_y;
      if (above < -2.5 * 4096.0 || above > 1.0 * 4096.0) {
        continue;
      }
      vec2 rel = world - wake[i].xz / 4096.0;  // meters
      float dist = length(rel);
      float age = wake[i].w;
      float x = dist - (0.3 + 1.4 * age);
      float band = exp(-x * x / 0.15) * (1.0 - age / 2.0);
      if (band > 0.001) {
        slope += rel / max(dist, 0.05) * (band * 0.45 * wake_strength * sin(x * 10.0));
      }
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
      if (sun_dir.w > 0.0 && seabed_level && water_caustics > 0.0) {
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
    vec3 body = mix(water_deep_color, water_shallow_color, clear_water) * shade;
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
      vec3 q = p;
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
          vec2 edge = min(quv, 1.0 - quv);
          ssr_hit = smoothstep(0.0, 0.05, min(edge.x, edge.y)) *
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

    // sun glints: a tight highlight plus a broad sheen toward the sun
    if (sun_dir.w > 0.0) {
      float sr = max(dot(r, sun_dir.xyz), 0.0);
      float glint = pow(sr, 900.0) * 7.0 + pow(sr, 80.0) * 0.25;
      wc += vec3(1.0, 0.93, 0.78) * glint * sun_dir.w;
    }

    // foam where the water meets the shore, moving with the waves
    if (!water_deep && seabed_level && depth_m < 0.8) {
      float wave = 0.5 + 0.7 * gnoise(world * 1.4 + vec2(t * 0.35, -t * 0.2)) +
                   0.35 * gnoise(world * 3.7 - vec2(t * 0.5, t * 0.3));
      float foam = smoothstep(0.8, 0.05, depth_m) * smoothstep(0.25, 0.75, wave + 0.6 - depth_m);
      wc = mix(wc, vec3(0.92, 0.94, 0.9), foam * 0.55);
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
  float ao = wsurf > 0.0 ? 1.0 : texture(tex_T2, tex_coord).r;
  if (ao_debug != 0) {
    // right half: distance (grey: log10 of meters / 4, so 1 km = 0.75), sky in blue
    float lm = z > 0.0 ? log(z / 4096.0) / log(10.0) / 4.0 : -1.0;
    color = tex_coord.x > 0.5 ? (z > 0.0 ? vec4(vec3(lm), 1.0) : vec4(0.0, 0.0, 1.0, 1.0)) : vec4(vec3(ao), 1.0);
    return;
  }
  if (normal_debug != 0 && z > 0.0) {
    // the level's own normals in color, the depth-rebuilt ones in grey
    // left half: the normals used (level's own in color, depth-rebuilt in grey); right half: the
    // depth-rebuilt ones in color, to compare
    vec3 nn = surface_normal(tex_coord, p);
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
  vec3 wet_n = vec3(0.0);
  if (weather.w > 0.0 && z > 0.0 && wsurf == 0.0 && z < 150.0 * 4096.0) {
    wet_n = surface_normal(tex_coord, p);
    wet = weather.w * smoothstep(0.5, 0.85, dot(wet_n, up_view)) *
          (1.0 - smoothstep(100.0 * 4096.0, 150.0 * 4096.0, z));
    c *= 1.0 - 0.1 * weather.x - 0.38 * wet;
  }

  // footprints and ruts (left where the game said the ground is soft): pressed into the flat ground
  // around them, not into grass, darker inside with a lighter rim of pushed-up sand, fading over
  // half a minute
  if (print_count > 0 && z > 0.0 && z < 40.0 * 4096.0 && wsurf == 0.0) {
    float sandy = (1.0 - smoothstep(1.05, 1.2, c.g / max(c.r, 0.01))) *
                  smoothstep(0.08, 0.2, dot(c, vec3(0.333)));
    vec3 nn = surface_normal(tex_coord, p);
    float level_ground = smoothstep(0.75, 0.9, dot(nn, up_view));
    if (sandy * level_ground > 0.0) {
      vec3 w = view_to_world * p + cam_pos;
      float dent = 0.0, rim = 0.0;
      for (int i = 0; i < print_count; i++) {
        vec3 d = (w - prints[i].xyz) / 4096.0;  // meters
        float kind = print_info[i].y;
        // feet: the ground is at the print; wheels: under the seat
        if (d.y > 0.4 || d.y < (kind > 1.5 ? -2.5 : -0.4) || dot(d.xz, d.xz) > 9.0) {
          continue;
        }
        vec2 f = vec2(sin(prints[i].w), cos(prints[i].w));
        float along = dot(d.xz, f);
        float side = dot(d.xz, vec2(f.y, -f.x));
        float fade = 1.0 - smoothstep(12.0, 30.0, print_info[i].x);
        if (kind > 1.5) {
          // two tracks 1.7 m apart, each piece reaching the next
          float t = abs(abs(side) - 0.85);
          float len = 1.0 - smoothstep(0.25, 0.4, abs(along));
          dent = max(dent, (1.0 - smoothstep(0.07, 0.17, t)) * len * fade * 0.8);
          rim = max(rim, smoothstep(0.12, 0.18, t) * (1.0 - smoothstep(0.18, 0.28, t)) * len * fade);
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
    vec3 nn = surface_normal(tex_coord, p);
    float ndl = dot(nn, sun_dir.xyz);
    if (ndl > 0.0) {
      // pushed off the surface along its normal (more far away, where a texel is bigger)
      vec3 w = view_to_world * (p + nn * (0.08 * 4096.0 + z * 0.002)) + cam_pos;
      float sh = sun_shadow(w) * smoothstep(0.0, 0.3, ndl);
      c *= 1.0 - shadow_strength * sh;
    }
  }

  // dynamic lights: each glow lights the surfaces around it that face it, tinted by the surface's
  // own color (the scene's color stands in for its albedo), with a smooth falloff to its radius
  if (light_count > 0 && z > 0.0 && wsurf == 0.0) {
    vec3 nn = surface_normal(tex_coord, p);
    vec3 added = vec3(0.0);
    // per-pixel jitter of the shadow steps (the TAA averages it)
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) +
                         haze.w * 7.13);
    vec3 from = p + nn * (0.05 * 4096.0 + z * 0.003);
    vec3 glints = vec3(0.0);  // the lights' highlights on wet ground
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
        if (light_shadow > 0.0 && i < 8 && k * k * lit > 0.02) {
          lit *= 1.0 - light_shadow * light_blocked(from, light_pos[i].xyz, jitter);
        }
        added += light_color[i].rgb * (k * k) * lit;
        if (wet > 0.0) {
          vec3 hv = normalize(to_light / max(d, 1.0) + vdir);
          glints += light_color[i].rgb * k * pow(max(dot(nn, hv), 0.0), 70.0) * lit;
        }
      }
    }
    c += (c + 0.12) * added * ao;
    c += glints * wet * 1.6;
  }

  // indirect light: what the surroundings send, tinted by this surface's own color
  if (gi_strength > 0.0 && wsurf == 0.0) {
    vec3 gi = texture(tex_T7, tex_coord).rgb;
    c += gi * gi_strength * (0.35 + c) * ao;
  }

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
      vec3 nn = surface_normal(tex_coord, p);
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
    }
  }

  // wet ground: puddles in the dips of a world-space noise, raindrop rings in them, and the
  // surroundings reflected (strongest in the puddles and at grazing angles: the neon on a wet street)
  if (wet > 0.01) {
    vec3 ww = (view_to_world * p + cam_pos) / 4096.0;
    // wide puddles: low-frequency dips with a ragged rim
    float pd = gnoise(ww.xz * 0.22) + 0.45 * gnoise(ww.xz * 0.7 + 7.1) + 0.12 * gnoise(ww.xz * 3.1 + 2.3);
    float spread = 0.22 * weather2.x;  // a thunderstorm floods them wider
    float puddle = smoothstep(-0.12 - spread, 0.02 - spread, pd) * smoothstep(0.85, 0.95, dot(wet_n, up_view)) *
                   (1.0 - smoothstep(30.0 * 4096.0, 60.0 * 4096.0, z));
    c *= 1.0 - 0.25 * puddle * wet;
    vec2 rip = rain_ripples(ww.xz, weather.z) * weather.x * (1.0 - smoothstep(8.0 * 4096.0, 20.0 * 4096.0, z));
    // outside the puddles the wet ground is rough: a fine noise breaks its reflection up
    vec2 rough = vec2(gnoise(ww.xz * 9.0), gnoise(ww.xz * 9.0 + 5.3)) * (1.0 - puddle) *
                 (1.0 - smoothstep(10.0 * 4096.0, 30.0 * 4096.0, z));
    float tw = weather.z;
    vec2 wave = vec2(gnoise(ww.xz * 2.2 + vec2(tw * 0.35, tw * 0.2)),
                     gnoise(ww.xz * 2.2 + vec2(5.0 - tw * 0.25, tw * 0.3))) * 0.07 +
                vec2(gnoise(ww.xz * 7.0 + tw * 0.6), gnoise(ww.xz * 7.0 + 3.3 - tw * 0.5)) * 0.025;
    vec2 bend = rip * (0.15 + 0.6 * puddle) + rough * 0.35 + wave * puddle;
    vec3 n2 = normalize(wet_n + right_view * bend.x + fwd_view * bend.y);
    vec3 vdir = normalize(p);
    vec3 r = reflect(vdir, n2);
    float facing = clamp(dot(-vdir, n2), 0.0, 1.0);
    float f = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
    float hit;
    vec3 refl = trace_reflection(p, r, z, hit);
    // a sheen, not a mirror: the ground's own color stays, dimmer reflections outside the puddles
    refl = mix(refl, c, 0.25 * (1.0 - puddle));
    float amount = wet * (0.2 + 0.4 * puddle) * clamp(f * 1.3 + 0.12 * puddle + 0.03, 0.0, 1.0) *
                   (1.0 - smoothstep(60.0 * 4096.0, 150.0 * 4096.0, z));
    c = mix(c, refl, amount);
    // splashes: drops hitting the ground, each a bright crown spreading for a tenth of a second
    if (weather.x > 0.0 && z < 15.0 * 4096.0) {
      float spl = 0.0;
      for (int k = 0; k < 2; k++) {
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
      vec3 nn = surface_normal(tex_coord, p);
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
    vec3 nn = surface_normal(tex_coord, p);
    float ndl = dot(nn, sun_dir.xyz);
    float k = relight * (1.0 - smoothstep(150.0 * 4096.0, 300.0 * 4096.0, z));
    c *= 1.0 + k * clamp(ndl, -0.6, 1.0) * 0.35;
  }

  // the sky: ours by day over the game's (its painted clouds go), the game's at night; the lightning
  // bolt behind the clouds; the cloud volume over both; greyer under rain
  // (the game's painted cloud cards kilometers out above the horizon count as sky by day: ours
  // replaces them)
  float bolt_light = 0.0;
  float sky_k = z == 0.0 ? 1.0
                         : smoothstep(2800.0 * 4096.0, 4000.0 * 4096.0, z) *
                               smoothstep(0.04, 0.09, world_dir(ray).y) * (1.0 - clouds.w) *
                               step(0.0, clouds.x - 0.001);
  if (sky_k > 0.0 && wsurf == 0.0) {
    vec3 c0 = c;
    if (clouds.x > 0.0) {
      vec3 sky = sky_color(ray);
      c = mix(c, sky, clouds.x * (1.0 - clouds.w));
      vec4 cl = cloud_volume(ray, pixel_angle() / 6.2832, mix(sky, c, clouds.w));
      c = mix(c, c * cl.a + cl.rgb, clouds.x);
      if (bolt.w > 0.0) {
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
    c = mix(c, mix(fog_color, vec3(luma(fog_color)) * vec3(0.82, 0.94, 1.15), 0.55 * weather.x), amount * fog.z);
  }
  // rain mist: the distance goes grey
  if (weather.x > 0.0 && z > 0.0) {
    vec3 grey = vec3(luma(fog_color)) * vec3(0.82, 0.94, 1.15);  // the fog's brightness, cold, not its tint
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
    c = mix(c, fc, ground_fog(ray, dist, low_fog) * 0.9);
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
    c = mix(c, sand, cover * 0.94);
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
    c = c * v.a + v.rgb;
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
    for (int j = 0; j < 18; j++) {
      float s = fire_seed[i] * 17.0 + float(j) * 7.31;
      float h1 = fract(sin(s * 12.9898) * 43758.5453);
      float h2 = fract(sin(s * 78.233) * 43758.5453);
      float h3 = fract(sin(s * 37.719) * 43758.5453);
      float h4 = fract(sin(s * 93.989) * 43758.5453);
      float life = 1.4 + h1 * 1.6;  // seconds
      float phase = fract(fire.z / life + h2);
      // a new ember each cycle starts somewhere else in the flame
      float cycle = floor(fire.z / life + h2);
      float k1 = fract(sin((s + cycle) * 41.17) * 43758.5453) - 0.5;
      float k2 = fract(sin((s + cycle) * 19.31) * 43758.5453) - 0.5;
      float rise = phase * fs * (2.0 + 3.0 * h3);
      float sway = sin(fire.z * (1.5 + h4 * 2.0) + h1 * 6.283) * fs * 0.3 * phase;
      vec3 ep = fp + up_view * (rise + fs * 0.2) + right_view * (k1 * fs * 0.9 + sway) +
                fwd_view * (k2 * fs * 0.9);
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
      c += hot * (glow * fade * flick * fire.y * 2.4);
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
    for (int j = 0; j < 40; j++) {
      float s = blast_seed[i] * 13.7 + float(j) * 3.17;
      float h1 = fract(sin(s * 12.9898) * 43758.5453);
      float h2 = fract(sin(s * 78.233) * 43758.5453);
      float h3 = fract(sin(s * 37.719) * 43758.5453);
      float h4 = fract(sin(s * 93.989) * 43758.5453);
      float life = 0.5 + h4 * 0.9;
      if (age > life) {
        continue;
      }
      // out and mostly up, in world axes
      vec3 dw = normalize(vec3(h1 - 0.5, 0.25 + h2 * 0.75, h3 - 0.5));
      vec3 dir = right_view * dw.x + up_view * dw.y + fwd_view * dw.z;
      float speed = (7.0 + 13.0 * fract(h1 * 7.3)) * 4096.0;
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
      vec3 hot = mix(vec3(1.0, 0.92, 0.65), vec3(1.0, 0.35, 0.05), cool);
      c += hot * glow * (1.0 - cool) * 2.2 * blast_sparks;
    }
  }

  // halos: in rain, fog or dust the air around each light glows (scattering), less where the
  // light is behind something
  float air = weather.x * 0.8 + weather2.y + weather.y * 0.5 + weather2.x * 0.3;
  if (air > 0.02 && light_count > 0) {
    vec3 halo = vec3(0.0);
    float scene_dist = z > 0.0 ? length(p) : 1e12;
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

  // bloom and god rays
  c += texture(tex_T1, tex_coord).rgb * bloom_strength;
  c += texture(tex_T4, tex_coord).rgb * shafts_strength;

  // highlights roll off instead of clipping (bloom can push past 1)
  c = c / (1.0 + max(c - 0.85, 0.0) * 1.2);

  // grading: the area's tint (shadows lifted toward one color, highlights toward another), then
  // saturation, gentle S-curve, warm highlights / cool shadows
  // rain: colder, the colors kept (a touch richer, like the wet slums at night)
  c *= mix(vec3(1.0), vec3(0.9, 0.98, 1.1), weather.x);
  c = max(mix(vec3(luma(c)), c, 1.0 + 0.1 * weather.x), 0.0);
  c = c * grade_gain + grade_lift * (1.0 - clamp(c, 0.0, 1.0));
  float l = luma(c);
  c = max(mix(vec3(l), c, saturation * (1.0 + 0.12 * weather2.z)), 0.0);
  vec3 curve = c * c * (3.0 - 2.0 * c);
  c = mix(c, curve, contrast);
  c += warmth * vec3(1.0, 0.25, -0.75) * (l - 0.35);

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
