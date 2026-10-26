#version 410 core

// Jak Enhanced Trilogy: bloom downsample (13-tap, as in Call of Duty: AW), with a soft-knee
// bright pass on the first level.

uniform sampler2D tex_T0;
uniform vec2 texel;  // source texel size
uniform float threshold;
uniform float knee;
uniform int first_pass;

in vec2 tex_coord;
out vec4 color;

vec3 bright(vec3 c) {
  float lum = max(c.r, max(c.g, c.b));
  float soft = clamp(lum - threshold + knee, 0.0, 2.0 * knee);
  soft = soft * soft / (4.0 * knee + 1e-4);
  float contrib = max(soft, lum - threshold) / max(lum, 1e-4);
  return c * contrib;
}

void main() {
  vec2 uv = tex_coord;
  vec3 a = texture(tex_T0, uv + texel * vec2(-2, -2)).rgb;
  vec3 b = texture(tex_T0, uv + texel * vec2(0, -2)).rgb;
  vec3 c = texture(tex_T0, uv + texel * vec2(2, -2)).rgb;
  vec3 d = texture(tex_T0, uv + texel * vec2(-2, 0)).rgb;
  vec3 e = texture(tex_T0, uv).rgb;
  vec3 f = texture(tex_T0, uv + texel * vec2(2, 0)).rgb;
  vec3 g = texture(tex_T0, uv + texel * vec2(-2, 2)).rgb;
  vec3 h = texture(tex_T0, uv + texel * vec2(0, 2)).rgb;
  vec3 i = texture(tex_T0, uv + texel * vec2(2, 2)).rgb;
  vec3 j = texture(tex_T0, uv + texel * vec2(-1, -1)).rgb;
  vec3 k = texture(tex_T0, uv + texel * vec2(1, -1)).rgb;
  vec3 l = texture(tex_T0, uv + texel * vec2(-1, 1)).rgb;
  vec3 m = texture(tex_T0, uv + texel * vec2(1, 1)).rgb;

  vec3 sum = e * 0.125;
  sum += (a + c + g + i) * 0.03125;
  sum += (b + d + f + h) * 0.0625;
  sum += (j + k + l + m) * 0.125;

  if (first_pass != 0) {
    sum = bright(sum);
  }
  color = vec4(sum, 1.0);
}
