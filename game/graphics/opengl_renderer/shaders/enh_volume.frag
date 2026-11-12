#version 410 core

// Jak Enhanced Trilogy: volumetric mist. Half resolution: along each view ray, up to the scene's depth,
// a ground mist (thicker low, drifting with the wind as 3D noise) scatters the fog color and the
// sun. Each sample looks a few steps toward the sun through the depth buffer: where a wall or a
// roof is in the way the mist stays dark, so light shafts stream through alleys and arches.
// The start of the march is jittered per pixel and per frame; the TAA smooths it out.
// Output: rgb = light scattered toward the camera, a = how much of the scene still shows through.

uniform sampler2D tex_T0;  // depth (full resolution)
uniform sampler2D tex_T2;  // the near sea's surface depth (0 where there's none)
uniform vec4 proj;         // a, b, fx, fy (see enh_ssao.frag)
uniform mat3 view_to_world;
uniform vec3 cam_pos;      // world, game units
uniform vec3 up_view;      // world up in view space
uniform vec4 sun_dir;      // view space, w: strength (0 = no sun)
uniform vec3 fog_color;
uniform vec4 mist;         // density at the ground (/m), height scale (m), max distance (m), time (s)
uniform vec4 mist_shape;   // ground below the camera (m), noise amount, sun intensity, anisotropy
uniform float frame;
uniform sampler2DShadow tex_T1;  // sun shadow map
uniform int shadow_on;
uniform mat4 light_mvp;          // world (game units) -> shadow map
// the nearest lamps and fires (view space, game units; w: reach) lighting a thin air of their own,
// with no mist (volume.local_lights): halos and glow around them. vl_air: density (/m), gain, the
// halo's core (m), shape (1: compact halo integrated along the ray, falling to exactly 0 at the
// light's halo radius; 0: the old marched lobe, which over a lamp-lit alley summed to a pale veil)
uniform int vl_count;
uniform vec4 vl_pos[8];
uniform vec4 vl_color[8];
uniform vec4 vl_air;

in vec2 tex_coord;
out vec4 color;

const int STEPS = 24;
const float METER = 4096.0;

float hash(vec3 p) {
  p = fract(p * 0.3183099 + 0.1);
  p *= 17.0;
  return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise(vec3 x) {
  vec3 i = floor(x);
  vec3 f = fract(x);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(hash(i + vec3(0, 0, 0)), hash(i + vec3(1, 0, 0)), f.x),
                 mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), f.x), f.y),
             mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), f.x),
                 mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), f.x), f.y),
             f.z);
}

float view_z(float d) {
  return d > 1e-7 ? proj.y / (d * 2.0 - 1.0 - proj.x) : 0.0;
}

// mist density (/m) at a view-space point (game units)
float density(vec3 p) {
  float h = dot(p, up_view) / METER + mist_shape.x;  // meters above the assumed ground
  float d = mist.x * exp(-max(h, 0.0) / mist.y);
  if (mist_shape.y > 0.0) {
    vec3 w = (view_to_world * p + cam_pos) / METER;
    vec3 drift = vec3(0.6, 0.05, 0.25) * mist.w;
    float n = noise(w * 0.06 + drift * 0.06) * 0.6 + noise(w * 0.17 + drift * 0.12) * 0.4;
    d *= mix(1.0, smoothstep(0.2, 0.85, n) * 2.0, mist_shape.y);
  }
  return d;
}

// is the sun hidden from this view-space point? (screen-space march through the depth buffer)
float sun_visible(vec3 p) {
  if (shadow_on != 0) {
    vec4 l = light_mvp * vec4(view_to_world * p + cam_pos, 1.0);
    vec3 s = l.xyz * 0.5 + 0.5;
    if (all(greaterThan(s, vec3(0.0))) && all(lessThan(s, vec3(1.0)))) {
      return texture(tex_T1, vec3(s.xy, s.z - 0.0002));
    }
  }
  const float lengths[8] = float[](0.5, 1.5, 3.0, 5.0, 8.0, 12.0, 18.0, 26.0);
  for (int i = 0; i < 8; i++) {
    vec3 q = p + sun_dir.xyz * lengths[i] * METER;
    if (q.z <= METER) {
      return 1.0;  // toward the camera: no depth to test against
    }
    vec2 uv = 0.5 * vec2(proj.z * q.x, proj.w * q.y) / q.z + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
      return 1.0;
    }
    float zs = view_z(texture(tex_T0, uv).r);
    // something solid in front of the point toward the sun (and not far in front: a thin wall,
    // not the whole foreground)
    if (zs > 0.0 && zs < q.z - 0.5 * METER && q.z - zs < 25.0 * METER) {
      return 0.0;
    }
  }
  return 1.0;
}

void main() {
  vec2 uv = tex_coord;
  float z = view_z(max(texture(tex_T0, uv).r, texture(tex_T2, uv).r));
  float max_z = mist.z * METER;
  vec2 ndc = uv * 2.0 - 1.0;
  vec3 dir = vec3(ndc.x / proj.z, ndc.y / proj.w, 1.0);  // view ray with z = 1
  // (lights only, no mist: the march need not go past the lights' 40 m)
  if (mist.x <= 0.0) {
    max_z = 60.0 * METER;
  }
  float end = (z > 0.0 ? min(z, max_z) : max_z);
  float ray_len = end * length(dir);
  vec3 step_v = dir * (end / float(STEPS));
  float ds = ray_len / float(STEPS) / METER;  // meters per step

  // interleaved gradient noise, moved every frame
  float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy + frame * vec2(5.588238, 5.588238),
                                               vec2(0.06711056, 0.00583715))));
  vec3 p = step_v * jitter;

  // light: fog color all around, the sun with a forward-scattering phase function
  vec3 ambient = fog_color * 0.6;
  vec3 sun_light = mix(fog_color, vec3(1.0, 0.93, 0.8), 0.7) * mist_shape.z * sun_dir.w;
  float g = mist_shape.w;
  float cos_t = dot(normalize(dir), sun_dir.xyz);
  float phase = (1.0 - g * g) / (4.0 * 3.14159 * pow(max(1.0 + g * g - 2.0 * g * cos_t, 1e-4), 1.5));
  phase = mix(0.0796, phase, 0.85);  // keep some isotropic part

  vec3 scattered = vec3(0.0);
  float trans = 1.0;
  vec3 dir_n = normalize(dir);
  // (no mist and the halos in closed form: no march at all)
  int steps = (mist.x > 0.0 || (vl_count > 0 && vl_air.w < 0.5)) ? STEPS : 0;
  for (int i = 0; i < steps; i++) {
    // the lamps' light in the air: each light's color by a smooth inverse square inside its reach,
    // scattered toward the camera with a mild forward lobe (Henyey-Greenstein g = 0.3)
    if (vl_count > 0 && vl_air.w < 0.5) {
      vec3 lsum = vec3(0.0);
      for (int k = 0; k < vl_count; k++) {
        vec3 d = p - vl_pos[k].xyz;
        float r = max(vl_pos[k].w, METER);
        float q = dot(d, d) / (r * r);
        if (q < 1.0) {
          float f = (1.0 - q) * (1.0 - q) / (1.0 + 8.0 * q);
          float ct = dot(d, -dir_n) * inversesqrt(max(dot(d, d), 1.0));
          float ph = 0.0703 / pow(1.09 - 0.6 * ct, 1.5);  // (1 - g^2) / (4 pi (1 + g^2 - 2 g ct)^1.5)
          lsum += vl_color[k].rgb * f * ph;
        }
      }
      scattered += trans * lsum * (vl_air.x * ds) * vl_air.y * 12.566;
    }
    float sigma = density(p);
    if (sigma > 1e-6) {
      float vis = sun_dir.w > 0.0 ? sun_visible(p) : 0.0;
      vec3 light = ambient + sun_light * vis * phase * 2.0;
      float t_step = exp(-sigma * ds);
      scattered += trans * light * (1.0 - t_step);
      trans *= t_step;
      if (trans < 0.02) {
        break;
      }
    }
    p += step_v;
  }
  // the lamps' halos, shape 1: per light, along the ray's visible part (camera to the scene) clipped
  // to the light's sphere (radius R), the integral of a2 / (a2 + d2) - a2 / (a2 + R2) (a: the core),
  // in closed form: bright around the lamp, 1/distance off it, exactly 0 from R out, so the dark
  // between the lamps keeps its black. Forward lobe by the angle to the light; no noise, no steps
  if (vl_count > 0 && vl_air.w >= 0.5) {
    vec3 lsum = vec3(0.0);
    float a = max(vl_air.z, 0.05);
    float a2 = a * a;
    float L = ray_len / METER;
    for (int k = 0; k < vl_count; k++) {
      vec3 c = vl_pos[k].xyz / METER;
      float R = max(vl_pos[k].w / METER, 1.0);
      float tc = dot(c, dir_n);
      float h2 = max(dot(c, c) - tc * tc, 0.0);
      if (h2 >= R * R) {
        continue;
      }
      float s = sqrt(R * R - h2);
      float t0 = max(tc - s, 0.0), t1 = min(tc + s, L);
      if (t1 <= t0) {
        continue;
      }
      float kk = sqrt(a2 + h2);
      float g = a2 * (atan((t1 - tc) / kk) - atan((t0 - tc) / kk)) / kk - a2 / (a2 + R * R) * (t1 - t0);
      float ct = 0.5 * tc * inversesqrt(max(dot(c, c), 1e-4));
      float ph = 0.0703 / pow(1.09 - 0.6 * ct, 1.5);
      lsum += vl_color[k].rgb * max(g, 0.0) * ph;
    }
    // (x2: about the old lobe's brightness at the lamp itself)
    scattered += lsum * vl_air.x * vl_air.y * 12.566 * 2.0 * (mist.x > 0.0 ? sqrt(trans) : 1.0);
  }
  color = vec4(scattered, trans);
}
