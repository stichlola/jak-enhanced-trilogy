#version 410 core

// Jak Enhanced Trilogy: temporal anti-aliasing. The 3D world is drawn with a different sub-pixel jitter
// every frame (Shader.cpp); this pass blends the frame into the history of the frames before,
// reprojected with the camera's move, so each pixel averages several sample positions. The
// history is clipped to the colors around the pixel this frame (variance clipping in YCoCg), so
// what moves on its own (Jak, enemies, particles) doesn't leave trails.

uniform sampler2D tex_T0;  // this frame (jittered)
uniform sampler2D tex_T1;  // history
uniform sampler2D tex_T3;  // depth
uniform vec4 proj;         // a, b, fx, fy (see enh_ssao.frag)
uniform vec2 texel;
uniform mat3 to_prev;      // view -> previous frame's view
uniform vec3 to_prev_offset;
uniform vec2 prev_f;       // previous fx, fy
uniform vec2 keep;         // history weight: still, fast
uniform int has_history;

in vec2 tex_coord;
out vec4 color;

vec3 to_ycocg(vec3 c) {
  return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b,
              -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 from_ycocg(vec3 c) {
  return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Catmull-Rom history fetch in 5 bilinear taps: stays sharp over many frames
vec3 history_at(vec2 uv) {
  vec2 size = 1.0 / texel;
  vec2 pos = uv * size;
  vec2 c1 = floor(pos - 0.5) + 0.5;
  vec2 f = pos - c1;
  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  vec2 w3 = f * f * (-0.5 + 0.5 * f);
  vec2 w12 = w1 + w2;
  vec2 tc0 = (c1 - 1.0) * texel;
  vec2 tc3 = (c1 + 2.0) * texel;
  vec2 tc12 = (c1 + w2 / w12) * texel;
  vec3 r = texture(tex_T1, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y) +
           texture(tex_T1, vec2(tc0.x, tc12.y)).rgb * (w0.x * w12.y) +
           texture(tex_T1, vec2(tc12.x, tc12.y)).rgb * (w12.x * w12.y) +
           texture(tex_T1, vec2(tc3.x, tc12.y)).rgb * (w3.x * w12.y) +
           texture(tex_T1, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);
  float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
  return max(r / wsum, vec3(0.0));
}

void main() {
  vec2 uv = tex_coord;
  vec3 cur = texture(tex_T0, uv).rgb;
  if (has_history == 0) {
    color = vec4(cur, 1.0);
    return;
  }

  // the colors around this pixel: mean and spread, and the nearest depth (the edge of what's in
  // front decides where the pixel came from)
  vec3 m1 = vec3(0.0);
  vec3 m2 = vec3(0.0);
  float d = 0.0;
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      vec2 o = vec2(x, y) * texel;
      vec3 s = to_ycocg(texture(tex_T0, uv + o).rgb);
      m1 += s;
      m2 += s * s;
      d = max(d, texture(tex_T3, uv + o).r);
    }
  }
  m1 /= 9.0;
  vec3 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));

  // where this pixel was last frame, if the world held still (the sky only turns with the camera)
  vec2 ndc = uv * 2.0 - 1.0;
  vec3 pp;
  if (d > 1e-7) {
    float z = proj.y / (d * 2.0 - 1.0 - proj.x);
    pp = to_prev * vec3(ndc.x * z / proj.z, ndc.y * z / proj.w, z) + to_prev_offset;
  } else {
    pp = to_prev * vec3(ndc.x / proj.z, ndc.y / proj.w, 1.0);
  }
  vec2 prev_uv = 0.5 * vec2(prev_f.x * pp.x, prev_f.y * pp.y) / pp.z + 0.5;
  if (pp.z <= 0.0 || any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
    color = vec4(cur, 1.0);
    return;
  }

  vec3 hist = to_ycocg(history_at(prev_uv));
  // clip the history toward the mean, into the box the neighbourhood spans
  vec3 lo = m1 - 1.25 * sigma;
  vec3 hi = m1 + 1.25 * sigma;
  vec3 center = 0.5 * (hi + lo);
  vec3 extent = max(0.5 * (hi - lo), vec3(1e-4));
  vec3 off = hist - center;
  vec3 unit = abs(off / extent);
  float k = max(unit.x, max(unit.y, unit.z));
  if (k > 1.0) {
    hist = center + off / k;
  }

  // keep less history when the pixel moves fast (it was resampled more, it blurs)
  float speed = length((uv - prev_uv) / texel);
  float weight = mix(keep.x, keep.y, clamp(speed / 12.0, 0.0, 1.0));
  // weighted by 1 / (1 + brightness) (Karis): a bright speck the jitter lands on in one frame of
  // several doesn't flash in and out (the shimmer of thin, far detail)
  vec3 cy = to_ycocg(cur);
  float wc = (1.0 - weight) / (1.0 + max(cy.x, 0.0));
  float wh = weight / (1.0 + max(hist.x, 0.0));
  vec3 c = (cy * wc + hist * wh) / max(wc + wh, 1e-5);
  color = vec4(from_ycocg(c), 1.0);
}
