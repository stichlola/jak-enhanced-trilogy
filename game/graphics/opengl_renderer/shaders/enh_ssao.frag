#version 430 core

// Jak Enhanced Trilogy: screen-space ambient occlusion (Alchemy AO) from the game's depth buffer.
// Positions are rebuilt with the projection saved by the background renderers:
// ndc_z = a + b / z, ndc_x = fx * x / z, ndc_y = fy * y / z.

uniform sampler2D tex_T0;  // depth
uniform sampler2D tex_T11;   // the geometry's world normals, their length the material's tag
uniform mat3 view_to_world;
uniform vec4 proj;         // a, b, fx, fy
uniform vec2 texel;
uniform float tune_ao_up0 = 0.5;   // ground this much facing up takes the geometry's normal...
uniform float tune_ao_up1 = 0.7;   // ...all of it from here (the depth's normal makes dark rows)
uniform float radius;        // game units
uniform float intensity;
uniform float max_distance;  // game units
uniform vec4 sun;            // xyz: key light direction in view space, w: shadow strength
uniform float sun_length;    // game units marched toward the light
uniform float close_up;      // cutscenes: nearer than this (game units) it all fades out (0 = off)
// GTAO (Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate Indirect Occlusion") in
// place of the near Alchemy taps: per slice the highest horizon on each side, the visible arc between
// them cosine-weighted against the normal. Same output; test switch until proven (0: Alchemy)
uniform float tune_ao_gtao = 0.0;
uniform float tune_ao_gtao_pow = 1.5;   // its visibility to this power (matches the old darkness)
uniform float tune_ao_gtao_thick = 0.25;
uniform float tune_ao_gtao_bias = 0.12;  // horizons opened by this much (radians)  // a sample this far behind its neighbors' horizon in r: thin, half counted

// ray-traced occlusion: short rays through the levels' hierarchies (see rt_trace in
// enh_composite.frag for the layout), world space in game units
layout(std430, binding = 0) readonly buffer RtNodes { vec4 rt_nodes[]; };
layout(std430, binding = 1) readonly buffer RtTris { vec4 rt_tris[]; };
uniform int rt_levels;       // 0: none (screen-space only)
uniform ivec4 rt_base[16];   // per level: node, triangle base
uniform vec3 cam_pos;        // world
uniform vec3 rt_ao;          // ray length, strength, how far from the camera (game units, -, game units)
uniform int rt_rays = 6;     // rays a pixel this frame
// accumulated over frames (post.rt_ao_temporal): a ray or two a pixel, turned each frame, mixed
// into last frame's result found where this point was then (the traced occlusion depends only on
// the world point and its normal, so reprojecting it is exact for the levels)
uniform int rt_temporal;
uniform int rt_history;      // 0: none (first frame, a cut)
uniform float rt_frame;
uniform sampler2D tex_T12;   // last frame's: r occlusion, g view distance (m)
uniform mat3 prev_view_to_world;
uniform vec3 prev_cam_pos;
uniform vec2 prev_f;         // last frame's fx, fy
uniform float tune_rt_keep = 0.9;   // the history's weight
uniform float tune_rt_reject = 0.03;  // a history this far off in distance (fraction) is another surface
// 1: half the pixels traced each frame (in turn). Off: no gain (JAK_ENHANCED_AO_CYCLE=2, wascity
// 640x360: the pass 0.11 ms with it, 0.09 ms without, 0.03 ms untraced): one ray a pixel is cheap
// already, the reprojection and the history fetch are most of what's left
uniform float tune_rt_checker = 0.0;
uniform int rt_every_pixel;  // (JAK_ENHANCED_AO_CYCLE=2's middle phase: the checkerboard off)

in vec2 tex_coord;
layout(location = 0) out float ao_out;
layout(location = 1) out vec2 rt_out;

const int SAMPLES = 16;
const float TURNS = 7.0;

float view_z(float d) {
  return proj.y / (d * 2.0 - 1.0 - proj.x);
}

vec3 view_pos(vec2 uv, float d) {
  float z = view_z(d);
  vec2 ndc = uv * 2.0 - 1.0;
  return vec3(ndc.x * z / proj.z, ndc.y * z / proj.w, z);
}

vec3 view_pos(vec2 uv) {
  return view_pos(uv, texture(tex_T0, uv).r);
}

// where along o + d * t the ray enters box (a, b), or 1e30 if it misses it before tmax
float rt_box(vec4 a, vec4 b, vec3 o, vec3 inv, float tmax) {
  vec3 t0 = (a.xyz - o) * inv;
  vec3 t1 = (b.xyz - o) * inv;
  vec3 tlo = min(t0, t1);
  vec3 thi = max(t0, t1);
  float tn = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
  float tf = min(min(thi.x, thi.y), min(thi.z, tmax));
  return tn <= tf ? tn : 1e30;
}

uniform int rt_unordered;  // test switch: the old traversal (children in fixed order)

// how far along o + d * t the nearest level triangle is (tmax: none). The nearer child first, so
// a hit shortens the ray early and the rest of the tree is skipped
float rt_nearest(vec3 o, vec3 d, float tmax) {
  vec3 dd = vec3(abs(d.x) < 1e-7 ? 1e-7 : d.x, abs(d.y) < 1e-7 ? 1e-7 : d.y,
                 abs(d.z) < 1e-7 ? 1e-7 : d.z);
  vec3 inv = 1.0 / dd;
  float best = tmax;
  int stack[32];
  float stack_t[32];
  for (int l = 0; l < rt_levels; l++) {
    int nb = rt_base[l].x;
    int tb = rt_base[l].y;
    // the whole level first: most are nowhere near a short ray
    if (rt_box(rt_nodes[nb * 2], rt_nodes[nb * 2 + 1], o, inv, best) > 1e29) {
      continue;
    }
    int sp = 0;
    stack[sp] = 0;
    stack_t[sp++] = 0.0;
    int steps = 0;
    while (sp > 0 && steps < 200) {
      steps++;
      sp--;
      int ni = stack[sp];
      if (stack_t[sp] > best) {
        continue;  // a hit since it was pushed is nearer than this whole node
      }
      vec4 a = rt_nodes[(nb + ni) * 2];
      vec4 b = rt_nodes[(nb + ni) * 2 + 1];
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
          }
        }
      } else if (sp < 30) {
        int c0 = ni + 1;
        int c1 = first;
        float t0 = rt_box(rt_nodes[(nb + c0) * 2], rt_nodes[(nb + c0) * 2 + 1], o, inv, best);
        float t1 = rt_box(rt_nodes[(nb + c1) * 2], rt_nodes[(nb + c1) * 2 + 1], o, inv, best);
        if (rt_unordered == 0 && t1 < t0) {
          int ci = c0; c0 = c1; c1 = ci;
          float tt = t0; t0 = t1; t1 = tt;
        }
        // the far one below the near one
        if (t1 < 1e29) {
          stack[sp] = c1;
          stack_t[sp++] = t1;
        }
        if (t0 < 1e29) {
          stack[sp] = c0;
          stack_t[sp++] = t0;
        }
      }
    }
  }
  return best;
}


// the open sky seen from world point w with world normal n (1 open, 0 shut in): cosine-weighted
// rays, a hit counts more the closer it is (linearly)
float rt_occlusion(vec3 w, vec3 n, float noise, float dist) {
  vec3 t = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
  vec3 b = cross(n, t);
  // off the surface: the depth buffer's position is coarser further away
  vec3 o = w + n * (0.03 * 4096.0 + 0.0015 * dist);
  float noise2 = fract(noise * 7.31 + 0.37);
  // (each frame further along a 2D low-discrepancy sequence, R2)
  float f1 = rt_temporal > 0 ? fract(rt_frame * 0.7548777) : 0.0;
  float f2 = rt_temporal > 0 ? fract(rt_frame * 0.5698403) : 0.0;
  float sum = 0.0;
  int rays = clamp(rt_rays, 1, 8);
  for (int i = 0; i < rays; i++) {
    float u1 = fract((float(i) + noise) / float(rays) + f1);
    float phi = 6.2831853 * fract(float(i) * 0.618034 + noise2 + f2);
    float r = sqrt(u1);
    vec3 d = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(1.0 - u1));
    float h = rt_nearest(o, d, rt_ao.x);
    float k = 1.0 - h / rt_ao.x;
    sum += k;
  }
  return 1.0 - rt_ao.y * sum / float(rays);
}

// visibility (0..1) of the hemisphere around n at p: 2 slices x 6 steps a side, r_uv the radius on
// screen, r_world in game units
float gtao(vec2 uv, float d, vec3 p, vec3 n, vec2 r_uv, float r_world, float noise, float spin) {
  const float PI = 3.14159265;
  const int SLICES = 2;
  const int STEPS = 6;
  vec3 view_v = normalize(-p);
  float falloff_range = 0.615 * r_world;
  float falloff_from = r_world - falloff_range;
  float vis = 0.0;
  for (int s = 0; s < SLICES; s++) {
    float phi = (float(s) + fract(noise * 1.618)) * PI / float(SLICES);
    vec2 dir = vec2(cos(phi), sin(phi));
    // (the slice's direction in view space from a real point at the same depth: the game's view space
    // may be mirrored on either axis)
    vec3 dir3 = view_pos(uv + dir * texel * 4.0, d) - p;
    vec3 ortho = dir3 - dot(dir3, view_v) * view_v;
    if (dot(ortho, ortho) < 1e-12) {
      vis += 1.0;
      continue;
    }
    ortho = normalize(ortho);
    vec3 axis = normalize(cross(ortho, view_v));
    vec3 pn = n - axis * dot(n, axis);
    float pn_len = length(pn);
    float cos_n = clamp(dot(pn, view_v) / max(pn_len, 1e-6), -1.0, 1.0);
    float n_ang = sign(dot(ortho, pn)) * acos(cos_n);
    // side 0 toward -dir, side 1 toward +dir
    float low0 = cos(n_ang - 0.5 * PI);
    float low1 = cos(n_ang + 0.5 * PI);
    float hc0 = low0, hc1 = low1;
    for (int j = 0; j < STEPS; j++) {
      float t = (float(j) + fract(noise + 0.37 * float(j + s))) / float(STEPS);
      t = t * t;  // (denser near the pixel: the creases)
      vec2 off = dir * r_uv * max(t, 1.5 * texel.x / max(r_uv.x, 1e-6));
      for (int side = 0; side < 2; side++) {
        vec2 suv = uv + (side == 0 ? -off : off);
        float sd = texture(tex_T0, suv).r;
        if (sd <= 1e-7) {
          continue;
        }
        vec3 h = view_pos(suv, sd) - p;
        float hl = length(h);
        float shc = dot(h / max(hl, 1e-6), view_v);
        float w = clamp((falloff_from - hl) / falloff_range + 1.0, 0.0, 1.0);
        float lo = side == 0 ? low0 : low1;
        shc = mix(lo, shc, w);
        // (thin things in front: a horizon that jumps past the one so far by much counts half)
        if (side == 0) {
          hc0 = shc > hc0 ? mix(shc, hc0, tune_ao_gtao_thick * step(0.5, shc - hc0)) : hc0;
        } else {
          hc1 = shc > hc1 ? mix(shc, hc1, tune_ao_gtao_thick * step(0.5, shc - hc1)) : hc1;
        }
      }
    }
    float h0 = -acos(clamp(hc0, -1.0, 1.0));
    float h1 = acos(clamp(hc1, -1.0, 1.0));
    // (opened by a few degrees, as the Alchemy taps skip what rises under 7 degrees: the depth's
    // rounding on ground seen at a grazing angle made a grey band toward the horizon)
    h0 = n_ang + max(h0 - tune_ao_gtao_bias - n_ang, -0.5 * PI);
    h1 = n_ang + min(h1 + tune_ao_gtao_bias - n_ang, 0.5 * PI);
    float sin_n = sin(n_ang);
    float arc0 = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - n_ang)) * 0.25;
    float arc1 = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - n_ang)) * 0.25;
    vis += pn_len * (arc0 + arc1);
  }
  return clamp(vis / float(SLICES), 0.0, 1.0);
}

void main() {
  vec2 uv = tex_coord;
  rt_out = vec2(1.0, 0.0);
  float d = texture(tex_T0, uv).r;
  if (d <= 1e-7) {
    ao_out = 1.0;  // sky / nothing drawn
    return;
  }
  vec3 p = view_pos(uv, d);
  float dist = abs(p.z);
  if (dist > max_distance) {
    ao_out = 1.0;
    return;
  }

  // normal from the neighbor on each axis with the smaller depth jump (no halos on edges)
  vec3 pr = view_pos(uv + vec2(texel.x, 0));
  vec3 pl = view_pos(uv - vec2(texel.x, 0));
  vec3 pu = view_pos(uv + vec2(0, texel.y));
  vec3 pd = view_pos(uv - vec2(0, texel.y));
  vec3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
  vec3 dy = abs(pu.z - p.z) < abs(p.z - pd.z) ? pu - p : p - pd;
  // on what's flat, from further apart: next to each other the depth's rounding tilts the normal,
  // a moire of dark rows across ground nearly level along the screen's rows
  vec3 pfr = view_pos(uv + vec2(32.0 * texel.x, 0));
  vec3 pfl = view_pos(uv - vec2(32.0 * texel.x, 0));
  vec3 pfu = view_pos(uv + vec2(0, 32.0 * texel.y));
  vec3 pfd = view_pos(uv - vec2(0, 32.0 * texel.y));
  if (abs(pfr.z + pfl.z - 2.0 * p.z) < 0.1 * length(pfr - pfl)) {
    dx = pfr - pfl;
  }
  if (abs(pfu.z + pfd.z - 2.0 * p.z) < 0.1 * length(pfu - pfd)) {
    dy = pfu - pfd;
  }
  vec3 n = normalize(cross(dx, dy));
  if (dot(n, -p) < 0.0) {
    n = -n;  // face the camera, whatever the handedness of the game's view space
  }
  // (from the depth, not the geometry's smoothed normals: on low-poly walls and crates those lean
  // out of the real face, so the flat surface around counts as blocking: dark flickering streaks)
  // (but the terrain's grid, tagged 0.85-0.95, is smooth: its own are the right ones)
  vec4 g = texture(tex_T11, uv);
  float g_tag = length(g.rgb * 2.0 - 1.0);
  if (g.a > 0.17 && g_tag > 0.3 && g_tag < 1.02) {  // (longer: a blended draw's leftover, no normal)
    vec3 gw = normalize(g.rgb * 2.0 - 1.0);
    vec3 gn = normalize(gw * view_to_world);  // world -> view (transpose)
    if (dot(gn, -p) < 0.0) {
      gn = -gn;
    }
    // the terrain always, and any ground facing up: the depth's normal goes far off on level ground
    // in rows at the same screen heights in every level (straight dark lines across the ground);
    // a crate's smoothed edge leans out of "up" and keeps the depth's
    // (from 0.5: gently sloped sand at 0.7-0.85 kept a dark row at y ~198 of 360 in the desert)
    float w = (g_tag > 0.82 && g_tag < 0.96) ? 1.0 : smoothstep(tune_ao_up0, tune_ao_up1, abs(gw.y));
    n = normalize(mix(n, gn, w));
  }

  // screen-space radius of the world radius at this depth (uv units)
  vec2 r_uv = 0.5 * radius * abs(vec2(proj.z, proj.w)) / dist;
  // keep it to a sensible part of the screen up close
  float r_scale = min(1.0, 0.12 / max(r_uv.x, 1e-6));
  r_uv *= r_scale;

  // per-pixel rotation (interleaved gradient noise), blurred away afterwards
  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  float spin = noise * 6.2831853;

  float sum = 0.0;
  float r2 = radius * radius * r_scale * r_scale;
  for (int i = 0; i < (tune_ao_gtao > 0.5 ? 0 : SAMPLES); i++) {
    float t = (float(i) + 0.5) / float(SAMPLES);
    float angle = t * TURNS * 6.2831853 + spin;
    vec2 suv = uv + r_uv * t * vec2(cos(angle), sin(angle));
    float sd = texture(tex_T0, suv).r;
    if (sd <= 1e-7) {
      continue;
    }
    vec3 v = view_pos(suv, sd) - p;
    float vv = dot(v, v);
    float falloff = 1.0 - smoothstep(0.5 * r2, r2, vv);
    // v.n / v.v scaled by the radius, so a fully blocking sample counts about 1
    // (only what rises over 7 degrees above the surface: shallow creases and bumps don't count)
    sum += falloff * max(0.0, dot(v, n) - 0.12 * sqrt(vv) - 0.002 * dist) / (vv + 0.01 * r2) * sqrt(r2);
  }

  float ao = max(0.0, 1.0 - 2.0 * intensity * sum / float(SAMPLES));
  if (tune_ao_gtao > 0.5) {
    float vis = gtao(uv, d, p, n, r_uv, radius * r_scale, noise, spin);
    ao = pow(vis, tune_ao_gtao_pow * intensity / 1.4);
  }

  // and a wider one, a few times the radius: rooms, alleys and the undersides of bridges darken
  // softly, not only the tight corners (gentler, and never below 0.6)
  {
    const int WIDE = 8;
    const float wide_k = 4.0;
    vec2 w_uv = r_uv * wide_k;
    float w_scale = min(1.0, 0.25 / max(w_uv.x, 1e-6));
    w_uv *= w_scale;
    float w_r2 = r2 * wide_k * wide_k * w_scale * w_scale;
    float w_sum = 0.0;
    for (int i = 0; i < WIDE; i++) {
      float t = (float(i) + 0.5) / float(WIDE);
      float angle = t * 3.0 * 6.2831853 + spin + 1.3;
      vec2 suv = uv + w_uv * sqrt(t) * vec2(cos(angle), sin(angle));
      float sd = texture(tex_T0, suv).r;
      if (sd <= 1e-7) {
        continue;
      }
      vec3 v = view_pos(suv, sd) - p;
      float vv = dot(v, v);
      float falloff = 1.0 - smoothstep(0.5 * w_r2, w_r2, vv);
      w_sum += falloff * max(0.0, dot(v, n) - 0.004 * dist) / (vv + 0.02 * w_r2) * sqrt(w_r2);
    }
    ao *= max(0.6, 1.0 - 0.9 * intensity * w_sum / float(WIDE));
  }

  // traced through the levels: corners and alleys darken from what's really there, on or off the
  // screen (the screen-space one still gives the characters, which aren't traced, their contact)
  if (rt_levels > 0 && dist < rt_ao.z) {
    vec3 wn = normalize(view_to_world * n);
    vec3 wp = view_to_world * p + cam_pos;
    float hist = -1.0;
    if (rt_temporal > 0) {
      vec3 pv = (wp - prev_cam_pos) * prev_view_to_world;  // world -> last frame's view (transpose)
      vec2 puv = 0.5 * prev_f * pv.xy / pv.z + 0.5;
      if (rt_history > 0 && all(greaterThan(puv, vec2(0.0))) && all(lessThan(puv, vec2(1.0)))) {
        vec2 h = texture(tex_T12, puv).rg;
        float pd = abs(pv.z) / 4096.0;
        if (abs(h.g - pd) < tune_rt_reject * pd + 0.05) {
          hist = h.r;
        }
      }
    }
    // (checkerboard of 8x8 tiles: half the pixels traced a frame, the others keep their history.
    // Whole tiles, as the GPU runs pixels in groups: one traced pixel in a group costs the group)
    ivec2 tile = ivec2(gl_FragCoord.xy) >> 3;
    bool skip = hist >= 0.0 && tune_rt_checker > 0.5 && rt_every_pixel == 0 &&
                ((tile.x + tile.y + int(rt_frame)) & 1) == 1;
    float rt = hist;
    if (!skip) {
      rt = clamp(rt_occlusion(wp, wn, noise, dist), 0.0, 1.0);
      if (hist >= 0.0) {
        rt = mix(rt, hist, tune_rt_keep);
      }
    }
    if (rt_temporal > 0) {
      rt_out = vec2(rt, dist / 4096.0);
    }
    rt = mix(rt, 1.0, smoothstep(0.7 * rt_ao.z, rt_ao.z, dist));
    ao = min(ao, max(rt, 0.0));
  }

  // screen-space sun shadow: march toward the light, shadowed if the depth buffer has a surface
  // in front of the ray (but not so far in front that it's just something unrelated)
  if (sun.w > 0.0) {
    // the game lights with dot(normal, dir), so dir points toward the light
    vec3 to_light = normalize(sun.xyz);
    float facing = dot(n, to_light);
    float shadow = 0.0;
    if (facing > 0.05) {
      const int STEPS = 20;
      vec3 start = p + n * (0.004 * dist);  // off the surface, more for far (coarser) depth
      for (int i = 1; i <= STEPS; i++) {
        float t = (float(i) + noise) / float(STEPS);
        vec3 q = start + to_light * sun_length * t * t;
        vec2 quv = 0.5 * vec2(proj.z * q.x, proj.w * q.y) / q.z + 0.5;
        if (quv.x < 0.0 || quv.y < 0.0 || quv.x > 1.0 || quv.y > 1.0) {
          break;
        }
        float qd = texture(tex_T0, quv).r;
        if (qd <= 1e-7) {
          continue;
        }
        float scene = abs(view_z(qd));
        float ray = abs(q.z);
        float in_front = ray - scene;
        if (in_front > 0.002 * ray && in_front < 0.35 * sun_length) {
          shadow = 1.0;
          break;
        }
      }
    }
    // faces turned away from the light are already dark in the game's lighting
    ao *= 1.0 - sun.w * shadow;
  }
  // fade out toward the max distance
  ao = mix(ao, 1.0, smoothstep(0.6 * max_distance, max_distance, dist));
  if (close_up > 0.0) {
    ao = mix(1.0, ao, smoothstep(0.5 * close_up, close_up, dist));
  }
  ao_out = ao;
}
