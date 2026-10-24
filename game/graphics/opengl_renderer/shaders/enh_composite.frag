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

uniform vec4 color_mult;
uniform vec4 color_add;

in vec2 tex_coord;
out vec4 color;

float luma(vec3 c) {
  return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

float view_z(float d) {
  return d > 1e-7 ? abs(proj.y / (d * 2.0 - 1.0 - proj.x)) : 0.0;
}

vec3 view_pos(vec2 uv, float d) {
  float z = proj.y / (d * 2.0 - 1.0 - proj.x);
  vec2 ndc = uv * 2.0 - 1.0;
  return vec3(ndc.x * z / proj.z, ndc.y * z / proj.w, z);
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
  if (motion.z > 0.0 && z > motion_keep.x) {
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

  // water reflections: where the ocean's surface is in front of the scene, trace the reflected
  // ray through the depth buffer (which has no water) and mix in what it hits, by Fresnel
  if (water_strength > 0.0) {
    float wd = texture(tex_T5, uv).r;
    float fd = texture(tex_T6, uv).r;
    // far sea: still the ocean's own depth at the end of the frame
    if (fd > 1e-7 && abs(fd - d) < 1e-7 && dot(up_view, view_pos(uv, fd)) < -4096.0) {
      wd = fd;
    }
    if (wd > d + 1e-7 || (wd > 1e-7 && wd == fd && dot(up_view, view_pos(uv, fd)) < -4096.0)) {
      vec3 wp = view_pos(uv, wd);
      // ripples: two moving wave trains on the world's horizontal plane
      vec2 world = vec2(dot(right_view, wp) + cam_pos.x, dot(fwd_view, wp) + cam_pos.z) / 4096.0;
      float t = haze.w;
      vec2 slope = 0.035 * vec2(sin(world.x * 1.3 + world.y * 0.4 + t * 1.7),
                                cos(world.y * 1.1 - world.x * 0.5 + t * 1.3)) +
                   0.015 * vec2(sin(world.y * 4.1 + t * 2.9), cos(world.x * 3.7 - t * 2.3));
      // smaller than a pixel far away: fade them out there, or they alias into moire bands
      slope *= 1.0 / (1.0 + abs(wp.z) / (60.0 * 4096.0));
      vec3 n = normalize(up_view + slope.x * right_view + slope.y * fwd_view);
      vec3 vdir = normalize(wp);
      vec3 r = reflect(vdir, n);
      float fresnel = 0.02 + 0.98 * pow(1.0 - clamp(dot(-vdir, n), 0.0, 1.0), 5.0);

      vec3 refl = vec3(0.0);
      float found = 0.0;
      float dist = abs(wp.z);
      float step_len = 0.25 * 4096.0 + dist * 0.01;
      vec3 q = wp;
      vec2 quv = uv;
      vec2 hit_uv = uv;
      vec2 sky_uv = vec2(-1.0);  // last sky (or cloud) texel the ray passed over
      for (int i = 0; i < 40; i++) {
        q += r * step_len;
        step_len *= 1.12;
        if (q.z <= 0.0) {
          break;  // came back toward the camera
        }
        quv = 0.5 * vec2(proj.z * q.x, proj.w * q.y) / q.z + 0.5;
        if (quv.x < 0.0 || quv.y < 0.0 || quv.x > 1.0 || quv.y > 1.0) {
          break;
        }
        float sd = texture(tex_T3, quv).r;
        float sfd = texture(tex_T6, quv).r;
        if (sd <= 1e-7 || (sfd > 1e-7 && abs(sfd - sd) < 1e-7)) {
          // sky, clouds (they keep the sky pass's depth too) or the sea itself: not a reflector
          if (sd <= 1e-7 || dot(up_view, view_pos(quv, sd)) > 0.0) {
            sky_uv = quv;
          }
          continue;
        }
        float scene_z = view_z(sd);
        if (q.z > scene_z && q.z - scene_z < step_len * 2.0 + 4096.0) {
          refl = texture(tex_T0, quv).rgb;
          hit_uv = quv;
          found = 1.0;
          break;
        }
      }
      // a real hit fades out toward the screen's edges (it may be cut off there)
      vec2 edge = min(hit_uv, 1.0 - hit_uv);
      float fade = found * smoothstep(0.0, 0.05, min(edge.x, edge.y));
      // missed everything: reflect the sky the ray last went over
      if (found == 0.0 && sky_uv.x >= 0.0) {
        refl = texture(tex_T0, sky_uv).rgb;
        fade = 1.0;
      }
      c = mix(c, refl, clamp(fresnel * 1.2, 0.0, 1.0) * fade * water_strength);
    }
  }

  // light depth of field on the far distance (samples nearer than the pixel are skipped)
  if (dof.z > 0.0 && z > 0.0) {
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
  float sharp = sharpen * (1.0 - blur);
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
  float ao = texture(tex_T2, tex_coord).r;
  if (ao_debug != 0) {
    color = vec4(vec3(ao), 1.0);
    return;
  }
  c *= ao;

  // fog: thicker with distance and below the camera
  if (fog.z > 0.0 && z > 0.0) {
    float amount = 1.0 - exp(-z * fog.x);
    amount *= clamp(exp(-height * fog.y), 0.0, 1.0);
    c = mix(c, fog_color, amount * fog.z);
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
