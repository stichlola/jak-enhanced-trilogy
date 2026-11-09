#version 410 core

// Jak Enhanced Trilogy: temporal anti-aliasing. The 3D world is drawn with a different sub-pixel jitter
// every frame (Shader.cpp); this pass blends the frame into the history of the frames before,
// reprojected with the camera's move, so each pixel averages several sample positions. The
// history is clipped to the colors around the pixel this frame (variance clipping in YCoCg), so
// what moves on its own (Jak, enemies, particles) doesn't leave trails.

uniform sampler2D tex_T0;  // this frame (jittered)
uniform sampler2D tex_T1;  // history
uniform sampler2D tex_T3;  // depth
uniform sampler2D tex_T4;  // the characters' motion vectors (uv offset to last frame; x >= 32: none)
uniform int has_motion = 0;
uniform float mv_stamp = -1.0;  // b of this frame's vectors (taa.motion_stamp), -1: none, all count
uniform float tune_taa_dbg = 0.0;  // 1: red where motion vectors were used (into the history too: a test view)
uniform vec4 proj;         // a, b, fx, fy (see enh_ssao.frag)
uniform vec2 texel;
uniform mat3 to_prev;      // view -> previous frame's view
uniform vec3 to_prev_offset;
uniform vec2 prev_f;       // previous fx, fy
uniform vec2 keep;         // history weight: still, fast
uniform int has_history;
uniform float tune_taa_off = 0.0;  // (test: this frame only, no history)
uniform float tune_dither = 1.0;   // dither the output to the 8-bit target (in steps; 0: off)
uniform float time_s;              // (turns the dither every frame)

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

// the history is RGBA16F, blitted into the 8-bit frame: a triangular +-1 step of interleaved
// gradient noise turned every frame, or the smooth average bands again in the sky and the shade
// (zero mean: what the history keeps of it averages out)
vec3 dither(vec3 c) {
  if (tune_dither <= 0.0) {
    return c;
  }
  vec2 fp = gl_FragCoord.xy + 5.588238 * mod(floor(time_s * 60.0), 64.0);
  float n1 = fract(52.9829189 * fract(dot(fp, vec2(0.06711056, 0.00583715))));
  float n2 = fract(52.9829189 * fract(dot(fp + vec2(47.0, 17.0), vec2(0.06711056, 0.00583715))));
  return c + (n1 + n2 - 1.0) * tune_dither / 255.0;
}

void main() {
  vec2 uv = tex_coord;
  vec3 cur = texture(tex_T0, uv).rgb;
  if (has_history == 0 || tune_taa_off > 0.5) {
    color = vec4(dither(cur), 1.0);
    return;
  }

  // the colors around this pixel: mean and spread, and the nearest depth (the edge of what's in
  // front decides where the pixel came from)
  vec3 m1 = vec3(0.0);
  vec3 m2 = vec3(0.0);
  float d = 0.0;
  vec2 near_o = vec2(0.0);
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      vec2 o = vec2(x, y) * texel;
      vec3 s = to_ycocg(texture(tex_T0, uv + o).rgb);
      m1 += s;
      m2 += s * s;
      float dd = texture(tex_T3, uv + o).r;
      if (dd > d) {
        d = dd;
        near_o = o;
      }
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
  // a character moving on its own: where its own bones put this point last frame (the nearest
  // pixel's around, as the depth above: its edge carries its motion)
  bool moved = false;
  if (has_motion != 0) {
    vec3 mvs = texture(tex_T4, uv + near_o).rgb;
    vec2 mv = mvs.rg;
    if (mv.x < 32.0 && (mv_stamp < 0.0 || abs(mvs.b - mv_stamp) < 0.5)) {
      prev_uv = uv + mv;
      pp.z = 1.0;
      moved = true;
    }
  }
  if (pp.z <= 0.0 || any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
    color = vec4(dither(cur), 1.0);
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
  if (tune_taa_dbg > 0.5) {
    float v = moved ? 1.0 : 0.0;
    color = vec4(mix(cur.ggg * 0.5, vec3(1.0, 0.0, 0.0), v), 1.0);
    return;
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
  color = vec4(dither(from_ycocg(c)), 1.0);
}
