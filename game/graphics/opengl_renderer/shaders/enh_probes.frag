#version 430 core

// Jak Enhanced Trilogy: the light probes (post.probes, tech-rt-probe-gi M2). A grid of points
// around the camera (kProbeX x kProbeY x kProbeZ, laid out as kProbeY slices side by side in x);
// each frame every probe traces a few rays all around it through the levels' hierarchies (the
// layout of rt_trace in enh_composite.frag) and blends them into what it had (the last frame's
// grid, moved by whole cells as the camera goes):
//   rgb: the light coming back from the levels around it (their color in the game's own baked
//        light at this hour), the mean over all directions: one bounce
//   a:   how much of the sky above it is open (the share of the upward rays that meet nothing)

layout(std430, binding = 0) readonly buffer RtNodes { vec4 rt_nodes[]; };
layout(std430, binding = 1) readonly buffer RtTris { vec4 rt_tris[]; };
layout(std430, binding = 2) readonly buffer RtPalettes { uint rt_pal[]; };
uniform int rt_levels;
uniform ivec4 rt_base[16];    // per level: node, triangle, palette, texture layer base (-1: none)
uniform sampler2DArray tex_T12;  // the levels' textures, small
uniform vec4 rt_tod[8];       // the time-of-day palettes' weights (per channel)
uniform sampler2D tex_T0;     // the last frame's probes
uniform vec4 grid;            // this grid's corner (world, game units), cell size
uniform ivec4 shift;          // xyz: its corner in the last grid's cells; w: 1 that one is usable
uniform ivec3 dims;           // probes along x, y, z
uniform float frame;          // turns the rays each frame
uniform float blend;          // the new frame's share
uniform float tmax;           // how far the rays go (game units)
uniform int rays;             // rays per probe a frame
uniform int every;            // each probe traced one frame in this many

out vec4 probe_out;

// the nearest level triangle along o + d * t: its color in the game's baked light (no haze)
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
        stack[sp++] = first;
        stack[sp++] = ni + 1;
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
    tex = textureLod(tex_T12, vec3(tuv, float(rt_base[best_level].w + int(layer))), 4.0).rgb;
  }
  int pi = (rt_base[best_level].z + int(floatBitsToUint(rt_tris[best_tri + 1].w))) * 8;
  vec4 tod = vec4(0.0);
  for (int k = 0; k < 8; k++) {
    tod += unpackUnorm4x8(rt_pal[pi + k]) * rt_tod[k];
  }
  color = min(tod.rgb, vec3(1.0)) * 2.0 * tex;
  dist = best;
  return true;
}

float hash(vec2 q) {
  return fract(sin(dot(q, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
  ivec2 t = ivec2(gl_FragCoord.xy);
  ivec3 c = ivec3(t.x % dims.x, t.x / dims.x, t.y);
  ivec3 pc = c + shift.xyz;
  bool keep = shift.w > 0 && all(greaterThanEqual(pc, ivec3(0))) && all(lessThan(pc, dims));
  // a band of 4 rows of the texture in every `every` traced a frame (whole bands: the GPU's
  // threads of a band take the same path), the others keep what they had
  if (keep && ((t.y >> 2) % every) != int(frame) % every) {
    probe_out = texelFetch(tex_T0, ivec2(pc.x + pc.y * dims.x, pc.z), 0);
    return;
  }
  vec3 o = grid.xyz + (vec3(c) + 0.5) * grid.w;
  // stratified over the sphere (rays rings from top to bottom, a golden-angle spiral), the whole
  // set turned and shifted each frame and per probe
  float h0 = fract(frame * 0.618034 + hash(vec2(c.xz) + float(c.y) * 17.0));
  float h1 = fract(frame * 0.754878 + hash(vec2(c.zx) * 1.37 + float(c.y) * 3.1));
  vec3 bounce = vec3(0.0);
  float up = 0.0, open = 0.0;
  for (int k = 0; k < rays; k++) {
    float y = 1.0 - 2.0 * (float(k) + h0) / float(rays);
    float r = sqrt(max(1.0 - y * y, 0.0));
    float phi = 2.39996323 * float(k) + 6.2831853 * h1;
    vec3 d = vec3(r * cos(phi), y, r * sin(phi));
    vec3 col;
    float dist;
    if (rt_trace(o, d, tmax, col, dist)) {
      bounce += col;
    } else if (y > 0.0) {
      open += y;  // (cosine weighted: the sky straight up counts most)
    }
    up += max(y, 0.0);
  }
  vec4 cur = vec4(bounce / float(rays), up > 0.0 ? open / up : 1.0);
  probe_out = keep ? mix(texelFetch(tex_T0, ivec2(pc.x + pc.y * dims.x, pc.z), 0), cur, blend)
                   : cur;
}
