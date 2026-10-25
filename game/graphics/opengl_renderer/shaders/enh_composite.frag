#version 410 core

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
uniform vec2 vol_texel;
uniform float gi_strength;
uniform int ao_debug;
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
uniform float relight;      // sun relight strength (0 = off)  // how much of our water color replaces the game's

uniform vec4 color_mult;
uniform vec4 color_add;

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

float view_z(float d) {
  return d > 1e-7 ? abs(proj.y / (d * 2.0 - 1.0 - proj.x)) : 0.0;
}

vec3 view_pos(vec2 uv, float d) {
  float z = proj.y / (d * 2.0 - 1.0 - proj.x);
  vec2 ndc = uv * 2.0 - 1.0;
  return vec3(ndc.x * z / proj.z, ndc.y * z / proj.w, z);
}

// how much of the sun the shadow map says is blocked at a world point (game units), 0..1
float sun_shadow(vec3 w) {
  vec4 l = light_mvp * vec4(w, 1.0);
  vec3 s = l.xyz * 0.5 + 0.5;
  if (any(lessThan(s, vec3(0.0))) || any(greaterThan(s, vec3(1.0)))) {
    return 0.0;
  }
  vec2 t = vec2(1.0 / 4096.0);
  float lit = 0.0;
  for (int y = -1; y <= 2; y++) {
    for (int x = -1; x <= 2; x++) {
      lit += texture(tex_T9, vec3(s.xy + (vec2(x, y) - 0.5) * t, s.z - 0.00012));
    }
  }
  // fade out toward the edge of the map
  vec2 e = abs(s.xy * 2.0 - 1.0);
  float fade = 1.0 - smoothstep(0.8, 1.0, max(e.x, e.y));
  return (1.0 - lit / 16.0) * fade;
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
  float height = dot(up_view, p);  // above (+) or below (-) the camera

  // water: the near sea's surface in front of the scene, or the far sea still showing. From here
  // on the surface is this pixel's depth (fog, blur, ...); seabed_d keeps what's under it.
  float wsurf = 0.0;
  float seabed_d = d;
  bool water_deep = true;
  if (water_strength > 0.0) {
    float wd = texture(tex_T5, uv).r;
    float fd = texture(tex_T6, uv).r;
    bool under_is_sea = fd > 1e-7 && abs(fd - d) < 1e-7 && dot(up_view, view_pos(uv, fd)) < -4096.0;
    if (wd > d + 1e-7) {
      wsurf = wd;
      water_deep = under_is_sea || d <= 1e-7;
    } else if (under_is_sea) {
      wsurf = fd;
    }
    if (wsurf > 0.0) {
      d = wsurf;
      z = view_z(d);
      p = view_pos(uv, d);
      height = dot(up_view, p);
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
    vec3 n = normalize(up_view + slope.x * right_view + slope.y * fwd_view);
    vec3 vdir = normalize(p);
    vec3 r = reflect(vdir, n);
    float facing = clamp(dot(-vdir, n), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);

    // how deep the water is here (meters), from the sea bed seen through it (near sea only)
    float depth_m = 40.0;
    if (!water_deep && seabed_d > 1e-7) {
      depth_m = max(dot(up_view, p - seabed_p), 0.0) / 4096.0;
    }
    float clear_water = exp(-depth_m / 2.0);

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
          float hit = smoothstep(0.0, 0.05, min(edge.x, edge.y)) *
                      (1.0 - smoothstep(50.0 * 4096.0, 80.0 * 4096.0, z));
          refl = mix(refl, texture(tex_T0, quv).rgb, hit);
          break;
        }
      }
    }
    wc = mix(wc, refl, clamp(fresnel * 1.2, 0.0, 1.0) * water_strength);

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
    c = wc;
  }

  // light depth of field on the far distance (samples nearer than the pixel are skipped)
  // (not on water: its pixels would blur back in the game's own, two-colored water)
  if (dof.z > 0.0 && z > 0.0 && wsurf == 0.0) {
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
    color = vec4(vec3(ao), 1.0);
    return;
  }
  c *= ao;

  // sun shadow map: surfaces facing the sun but blocked from it lose some light (the game's light is
  // baked per vertex and knows nothing of what stands between them and the sun)
  if (shadow_strength > 0.0 && sun_dir.w > 0.0 && z > 0.0 && wsurf == 0.0) {
    vec3 pr = view_pos(tex_coord + vec2(texel.x, 0), texture(tex_T3, tex_coord + vec2(texel.x, 0)).r);
    vec3 pl = view_pos(tex_coord - vec2(texel.x, 0), texture(tex_T3, tex_coord - vec2(texel.x, 0)).r);
    vec3 pu = view_pos(tex_coord + vec2(0, texel.y), texture(tex_T3, tex_coord + vec2(0, texel.y)).r);
    vec3 pd = view_pos(tex_coord - vec2(0, texel.y), texture(tex_T3, tex_coord - vec2(0, texel.y)).r);
    vec3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
    vec3 dy = abs(pu.z - p.z) < abs(p.z - pd.z) ? pu - p : p - pd;
    vec3 nn = normalize(cross(dx, dy));
    if (dot(nn, -p) < 0.0) {
      nn = -nn;
    }
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
    vec3 pr = view_pos(tex_coord + vec2(texel.x, 0), texture(tex_T3, tex_coord + vec2(texel.x, 0)).r);
    vec3 pl = view_pos(tex_coord - vec2(texel.x, 0), texture(tex_T3, tex_coord - vec2(texel.x, 0)).r);
    vec3 pu = view_pos(tex_coord + vec2(0, texel.y), texture(tex_T3, tex_coord + vec2(0, texel.y)).r);
    vec3 pd = view_pos(tex_coord - vec2(0, texel.y), texture(tex_T3, tex_coord - vec2(0, texel.y)).r);
    vec3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
    vec3 dy = abs(pu.z - p.z) < abs(p.z - pd.z) ? pu - p : p - pd;
    vec3 nn = normalize(cross(dx, dy));
    if (dot(nn, -p) < 0.0) {
      nn = -nn;
    }
    vec3 added = vec3(0.0);
    for (int i = 0; i < light_count; i++) {
      vec3 to_light = light_pos[i].xyz - p;
      float d = length(to_light);
      float r = light_pos[i].w;
      if (d < r) {
        float k = 1.0 - (d * d) / (r * r);
        float ndl = dot(nn, to_light / max(d, 1.0));
        added += light_color[i].rgb * (k * k) * clamp(ndl * 0.85 + 0.15, 0.0, 1.0);
      }
    }
    c += (c + 0.12) * added * ao;
  }

  // indirect light: what the surroundings send, tinted by this surface's own color
  if (gi_strength > 0.0 && wsurf == 0.0) {
    vec3 gi = texture(tex_T7, tex_coord).rgb;
    c += gi * gi_strength * (0.35 + c) * ao;
  }

  // sun relight: the game's light is baked per vertex; shade each pixel a little more by how it
  // faces the sun, with normals rebuilt from the depth buffer (the levels have none)
  if (relight > 0.0 && sun_dir.w > 0.0 && z > 0.0 && wsurf == 0.0) {
    vec3 pr = view_pos(tex_coord + vec2(texel.x, 0), texture(tex_T3, tex_coord + vec2(texel.x, 0)).r);
    vec3 pl = view_pos(tex_coord - vec2(texel.x, 0), texture(tex_T3, tex_coord - vec2(texel.x, 0)).r);
    vec3 pu = view_pos(tex_coord + vec2(0, texel.y), texture(tex_T3, tex_coord + vec2(0, texel.y)).r);
    vec3 pd = view_pos(tex_coord - vec2(0, texel.y), texture(tex_T3, tex_coord - vec2(0, texel.y)).r);
    vec3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
    vec3 dy = abs(pu.z - p.z) < abs(p.z - pd.z) ? pu - p : p - pd;
    vec3 nn = normalize(cross(dx, dy));
    if (dot(nn, -p) < 0.0) {
      nn = -nn;
    }
    float ndl = dot(nn, sun_dir.xyz);
    float k = relight * (1.0 - smoothstep(150.0 * 4096.0, 300.0 * 4096.0, z));
    c *= 1.0 + k * clamp(ndl, -0.6, 1.0) * 0.35;
  }

  // fog: thicker with distance and below the camera
  if (fog.z > 0.0 && z > 0.0) {
    float amount = 1.0 - exp(-z * fog.x);
    amount *= clamp(exp(-height * fog.y), 0.0, 1.0);
    c = mix(c, fog_color, amount * fog.z);
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
        float tz = view_z(texture(tex_T3, tuv).r);
        float bw = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
        float dz = (z > 0.0 && tz > 0.0) ? abs(tz - z) / max(z, 1.0) : (z == tz ? 0.0 : 1.0);
        float w = bw * exp(-dz * 20.0) + 1e-4;
        sum += texture(tex_T8, tuv) * w;
        wsum += w;
      }
    }
    vec4 v = sum / wsum;
    c = c * v.a + v.rgb;
  }

  // bloom and god rays
  c += texture(tex_T1, tex_coord).rgb * bloom_strength;
  c += texture(tex_T4, tex_coord).rgb * shafts_strength;

  // highlights roll off instead of clipping (bloom can push past 1)
  c = c / (1.0 + max(c - 0.85, 0.0) * 1.2);

  // grading: saturation, gentle S-curve, warm highlights / cool shadows
  float l = luma(c);
  c = max(mix(vec3(l), c, saturation), 0.0);
  vec3 curve = c * c * (3.0 - 2.0 * c);
  c = mix(c, curve, contrast);
  c += warmth * vec3(1.0, 0.25, -0.75) * (l - 0.35);

  // vignette
  vec2 v = tex_coord - 0.5;
  c *= 1.0 - vignette * smoothstep(0.25, 0.85, dot(v, v) * 2.2);

  color = vec4(clamp(c, 0.0, 1.0) * color_mult.rgb * color_mult.a, 1.0) + color_add;
}
