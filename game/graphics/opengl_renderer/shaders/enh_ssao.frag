#version 410 core

// Jak 3 Enhanced: screen-space ambient occlusion (Alchemy AO) from the game's depth buffer.
// Positions are rebuilt with the projection saved by the background renderers:
// ndc_z = a + b / z, ndc_x = fx * x / z, ndc_y = fy * y / z.

uniform sampler2D tex_T0;  // depth
uniform sampler2D tex_T11;   // the geometry's world normals (a = 0: none, rebuilt from depth)
uniform mat3 view_to_world;
uniform vec4 proj;         // a, b, fx, fy
uniform vec2 texel;
uniform float radius;        // game units
uniform float intensity;
uniform float max_distance;  // game units
uniform vec4 sun;            // xyz: key light direction in view space, w: shadow strength
uniform float sun_length;    // game units marched toward the light

in vec2 tex_coord;
out float ao_out;

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

void main() {
  vec2 uv = tex_coord;
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
  vec3 n = normalize(cross(dx, dy));
  vec4 g = texture(tex_T11, uv);
  if (g.a > 0.17) {
    n = normalize((g.rgb * 2.0 - 1.0) * view_to_world);  // world -> view (transpose)
  }
  if (dot(n, -p) < 0.0) {
    n = -n;  // face the camera, whatever the handedness of the game's view space
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
  for (int i = 0; i < SAMPLES; i++) {
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
    sum += falloff * max(0.0, dot(v, n) - 0.002 * dist) / (vv + 0.01 * r2) * sqrt(r2);
  }

  float ao = max(0.0, 1.0 - 2.0 * intensity * sum / float(SAMPLES));

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
  ao_out = ao;
}
