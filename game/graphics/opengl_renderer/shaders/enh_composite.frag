#version 410 core

// Jak 3 Enhanced: final composite of the frame on its way to the window. Light sharpen, bloom,
// color grading and vignette, then the game's own brightness/contrast (color_mult/color_add, as
// in post_processing.frag).

uniform sampler2D tex_T0;  // frame
uniform sampler2D tex_T1;  // bloom
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

  // bloom
  c += texture(tex_T1, uv).rgb * bloom_strength;

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
