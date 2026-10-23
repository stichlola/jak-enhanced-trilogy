#version 410 core

// Jak 3 Enhanced: final composite of the frame on its way to the window. Light sharpen, ambient
// occlusion, bloom,
// color grading and vignette, then the game's own brightness/contrast (color_mult/color_add, as
// in post_processing.frag).

uniform sampler2D tex_T0;  // frame
uniform sampler2D tex_T1;  // bloom
uniform sampler2D tex_T2;  // ambient occlusion
uniform int ao_debug;
uniform sampler2D tex_T3;  // depth
uniform vec4 proj;         // a, b, fx, fy (see enh_ssao.frag)
uniform vec3 dof;          // start, end (game units), max radius (pixels)
uniform sampler2D tex_T4;  // god rays
uniform float shafts_strength;
uniform vec2 texel;
uniform float bloom_strength;
uniform float saturation;
uniform float contrast;
uniform float warmth;
uniform float vignette;
uniform float sharpen;

uniform vec4 color_mult;
uniform vec4 color_add;

in vec2 tex_coord;
out vec4 color;

float luma(vec3 c) {
  return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
  vec2 uv = tex_coord;
  vec3 c = texture(tex_T0, uv).rgb;

  // light depth of field: far geometry gets a small disk blur. The sky (no depth) and anything
  // near stays sharp, and samples nearer than the pixel are skipped so nothing bleeds outward.
  if (dof.z > 0.0) {
    float d = texture(tex_T3, uv).r;
    float z = d > 1e-7 ? abs(proj.y / (d * 2.0 - 1.0 - proj.x)) : 0.0;
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
        float sz = sd > 1e-7 ? abs(proj.y / (sd * 2.0 - 1.0 - proj.x)) : 1e12;
        float w = sz > z * 0.85 ? 1.0 : 0.0;
        sum += texture(tex_T0, suv).rgb * w;
        wsum += w;
      }
      c = sum / wsum;
    }
  }

  // contrast-adaptive sharpen (AMD CAS, simplified): less where the local contrast is already high
  vec3 n = texture(tex_T0, uv + vec2(0, -texel.y)).rgb;
  vec3 s = texture(tex_T0, uv + vec2(0, texel.y)).rgb;
  vec3 w = texture(tex_T0, uv + vec2(-texel.x, 0)).rgb;
  vec3 e = texture(tex_T0, uv + vec2(texel.x, 0)).rgb;
  vec3 mn = min(c, min(min(n, s), min(w, e)));
  vec3 mx = max(c, max(max(n, s), max(w, e)));
  vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
  vec3 wgt = -amp * mix(0.125, 0.2, sharpen) * sharpen;
  c = clamp((c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0);

  // ambient occlusion
  float ao = texture(tex_T2, uv).r;
  if (ao_debug != 0) {
    color = vec4(vec3(ao), 1.0);
    return;
  }
  c *= ao;

  // bloom and god rays
  c += texture(tex_T1, uv).rgb * bloom_strength;
  c += texture(tex_T4, uv).rgb * shafts_strength;

  // highlights roll off instead of clipping (bloom can push past 1)
  c = c / (1.0 + max(c - 0.85, 0.0) * 1.2);

  // grading: saturation, gentle S-curve, warm highlights / cool shadows
  float l = luma(c);
  c = max(mix(vec3(l), c, saturation), 0.0);
  vec3 curve = c * c * (3.0 - 2.0 * c);
  c = mix(c, curve, contrast);
  c += warmth * vec3(1.0, 0.25, -0.75) * (l - 0.35);

  // vignette
  vec2 v = uv - 0.5;
  c *= 1.0 - vignette * smoothstep(0.25, 0.85, dot(v, v) * 2.2);

  color = vec4(clamp(c, 0.0, 1.0) * color_mult.rgb * color_mult.a, 1.0) + color_add;
}
