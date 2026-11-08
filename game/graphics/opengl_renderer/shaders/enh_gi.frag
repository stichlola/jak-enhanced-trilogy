#version 410 core

// Jak Enhanced Trilogy: one-bounce screen-space indirect light. Around each pixel, the scene colors
// within a few meters that face it are gathered (cosine weighted), so a sunlit floor tints the
// walls next to it and torch flames light their surroundings. Half resolution, blurred after.

uniform sampler2D tex_T0;  // depth
uniform sampler2D tex_T1;  // scene
uniform sampler2D tex_T11;   // the geometry's world normals (a = 0: none, rebuilt from depth)
uniform mat3 view_to_world;
uniform vec4 proj;         // a, b, fx, fy (see enh_ssao.frag)
uniform vec2 texel;        // of the depth texture
uniform float radius;      // game units
uniform float max_distance;

in vec2 tex_coord;
layout(location = 0) out vec4 gi_out;
// temporal (gi.temporal, as enh_ssao's): half the samples a frame, the other half last frame's,
// through the reprojected history (rgb: the gathered light, a: the view distance it was found at)
layout(location = 1) out vec4 hist_out;
uniform int gi_temporal;   // 0 off, 1 on (history in tex_T12), 2 on, no history yet
uniform int gi_frame;      // which half (0/1)
uniform sampler2D tex_T12;
uniform mat3 to_prev;
uniform vec3 to_prev_offset;
uniform vec2 prev_f;

const int SAMPLES = 12;

float view_z(float d) {
  return proj.y / (d * 2.0 - 1.0 - proj.x);
}

vec3 view_pos(vec2 uv, float d) {
  float z = view_z(d);
  vec2 ndc = uv * 2.0 - 1.0;
  return vec3(ndc.x * z / proj.z, ndc.y * z / proj.w, z);
}

void main() {
  vec2 uv = tex_coord;
  float d = texture(tex_T0, uv).r;
  gi_out = vec4(0.0);
  hist_out = vec4(0.0);
  if (d <= 1e-7) {
    return;
  }
  vec3 p = view_pos(uv, d);
  float dist = abs(p.z);
  if (dist > max_distance) {
    return;
  }

  vec3 pr = view_pos(uv + vec2(texel.x, 0), texture(tex_T0, uv + vec2(texel.x, 0)).r);
  vec3 pl = view_pos(uv - vec2(texel.x, 0), texture(tex_T0, uv - vec2(texel.x, 0)).r);
  vec3 pu = view_pos(uv + vec2(0, texel.y), texture(tex_T0, uv + vec2(0, texel.y)).r);
  vec3 pd = view_pos(uv - vec2(0, texel.y), texture(tex_T0, uv - vec2(0, texel.y)).r);
  vec3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
  vec3 dy = abs(pu.z - p.z) < abs(p.z - pd.z) ? pu - p : p - pd;
  vec3 n = normalize(cross(dx, dy));
  vec4 g = texture(tex_T11, uv);
  if (g.a > 0.17 && dot(g.rgb * 2.0 - 1.0, g.rgb * 2.0 - 1.0) < 1.04) {
    n = normalize((g.rgb * 2.0 - 1.0) * view_to_world);  // world -> view (transpose)
  }
  if (dot(n, -p) < 0.0) {
    n = -n;
  }

  vec2 r_uv = 0.5 * radius * abs(vec2(proj.z, proj.w)) / dist;
  float r_scale = min(1.0, 0.2 / max(r_uv.x, 1e-6));
  r_uv *= r_scale;
  float r2 = radius * radius * r_scale * r_scale;

  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  bool tmp = gi_temporal != 0;
  int t_step = tmp ? 2 : 1;
  for (int i = tmp ? gi_frame : 0; i < SAMPLES; i += t_step) {
    float t = (float(i) + 0.5) / float(SAMPLES);
    float angle = t * 7.0 * 6.2831853 + noise * 6.2831853;
    vec2 suv = uv + r_uv * sqrt(t) * vec2(cos(angle), sin(angle));
    float sd = texture(tex_T0, suv).r;
    if (sd <= 1e-7) {
      continue;  // sky: its light is already in the game's baked colors
    }
    vec3 v = view_pos(suv, sd) - p;
    float vv = dot(v, v);
    if (vv > r2) {
      continue;
    }
    float cosine = max(dot(n, v * inversesqrt(vv + 1.0)), 0.0);
    float w = cosine * (1.0 - vv / r2);
    sum += texture(tex_T1, suv).rgb * w;
    wsum += 1.0;
  }
  vec3 gi = sum * float(t_step) / float(SAMPLES);
  if (tmp) {
    vec3 now = gi;
    if (gi_temporal == 1) {
      vec3 pp = to_prev * p + to_prev_offset;
      vec2 puv = 0.5 * vec2(prev_f.x * pp.x, prev_f.y * pp.y) / pp.z + 0.5;
      if (pp.z > 0.0 && all(greaterThan(puv, vec2(0.0))) && all(lessThan(puv, vec2(1.0)))) {
        vec4 h = texture(tex_T12, puv);
        if (abs(h.a - abs(pp.z)) < 0.03 * abs(pp.z)) {
          gi = 0.5 * (now + h.rgb);
        }
      }
    }
    hist_out = vec4(now, dist);
  }
  gi_out = vec4(gi, 1.0);
}
