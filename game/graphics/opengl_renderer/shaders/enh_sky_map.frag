#version 430 core

// Jak Enhanced Trilogy: the sky map (post.sky_map). One ray a texel straight down through the
// levels' hierarchies (the layout of rt_trace in enh_composite.frag); the texel keeps the height
// of the first, topmost surface it meets: a roof, an awning, a tree's crown, else the ground.
// Drawn a band of rows at a time (the viewport), world space in game units.

layout(std430, binding = 0) readonly buffer RtNodes { vec4 rt_nodes[]; };
layout(std430, binding = 1) readonly buffer RtTris { vec4 rt_tris[]; };
uniform int rt_levels;
uniform ivec4 rt_base[16];   // per level: node, triangle base
uniform vec4 grid;           // corner x, z, cell size (game units), rays' start height (world y)
uniform float span;          // how far down they go (game units)

out vec4 top_out;

float rt_box(vec4 a, vec4 b, vec3 o, vec3 inv, float tmax) {
  vec3 t0 = (a.xyz - o) * inv;
  vec3 t1 = (b.xyz - o) * inv;
  vec3 tlo = min(t0, t1);
  vec3 thi = max(t0, t1);
  float tn = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
  float tf = min(min(thi.x, thi.y), min(thi.z, tmax));
  return tn <= tf ? tn : 1e30;
}

// the nearest hit along o + d * t (tmax: none), nearer child first
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
    if (rt_box(rt_nodes[nb * 2], rt_nodes[nb * 2 + 1], o, inv, best) > 1e29) {
      continue;
    }
    int sp = 0;
    stack[sp] = 0;
    stack_t[sp++] = 0.0;
    int steps = 0;
    while (sp > 0 && steps < 600) {
      steps++;
      sp--;
      int ni = stack[sp];
      if (stack_t[sp] > best) {
        continue;
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
        if (t1 < t0) {
          int ci = c0; c0 = c1; c1 = ci;
          float tt = t0; t0 = t1; t1 = tt;
        }
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

void main() {
  vec2 xz = grid.xy + (gl_FragCoord.xy) * grid.z;  // (the texel's center: gl_FragCoord is at .5)
  // the first four surfaces from the top (a roof, the arch's underside below it, the floor...),
  // each ray going on from just past the last hit; -1e30: none
  vec4 hits = vec4(-1e30);
  float y = grid.w;
  float left = span;
  for (int k = 0; k < 4; k++) {
    float t = rt_nearest(vec3(xz.x, y, xz.y), vec3(0.0, -1.0, 0.0), left);
    if (t >= left) {
      break;
    }
    hits[k] = y - t;
    y -= t + 0.03 * 4096.0;
    left -= t + 0.03 * 4096.0;
    if (left <= 0.0) {
      break;
    }
  }
  top_out = hits;
}
