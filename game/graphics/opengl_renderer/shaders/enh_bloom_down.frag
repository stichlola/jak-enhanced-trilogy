#version 410 core

// Jak Enhanced Trilogy: bloom downsample (13-tap, as in Call of Duty: AW), with a soft-knee
// bright pass on the first level.

uniform sampler2D tex_T0;
uniform vec2 texel;  // source texel size
uniform float threshold;
uniform float knee;
uniform int first_pass;
// the light effects (Jak 3): the scene before (T1) and after (T2) the sprite buckets. By day what
// they added glows on its own: the bright sand otherwise crosses the threshold as much as they do
uniform sampler2D tex_T1;
uniform sampler2D tex_T2;
uniform float fx_bloom;  // the day's weight on the effects' glow (0: off, at night as before)
uniform float tune_fx_bloom_gain = 1.6;
uniform float tune_fx_area_frac = 6.0;  // (as in enh_composite)
uniform float tune_fx_area0 = 0.3;
uniform float tune_fx_area1 = 0.7;
uniform float tune_fx_wash0 = 0.03;
uniform float tune_fx_wash1 = 0.08;
uniform float tune_fx_wash_lod = 1.5;

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
    if (fx_bloom > 0.0) {
      vec3 f1 = texture(tex_T2, uv).rgb;
      vec3 fx = max(f1 - texture(tex_T1, uv).rgb, 0.0);
      // (a channel the sum clipped kept less than it got: see enh_composite's fx_d)
      float fm = max(fx.r, max(fx.g, fx.b));
      fx = mix(fx, max(fx, vec3(fm)), smoothstep(vec3(0.94), vec3(0.99), f1));
      // (and only the compact ones: a wash of light over a room already blooms with the scene)
      float lod = log2(max(1.0, 1.0 / (texel.y * tune_fx_area_frac)));
      vec3 wide = max(textureLod(tex_T2, uv, lod).rgb - textureLod(tex_T1, uv, lod).rgb, 0.0);
      float lw = dot(wide, vec3(0.2126, 0.7152, 0.0722));
      float lf = dot(fx, vec3(0.2126, 0.7152, 0.0722));
      vec3 wider = max(textureLod(tex_T2, uv, lod + tune_fx_wash_lod).rgb -
                       textureLod(tex_T1, uv, lod + tune_fx_wash_lod).rgb, 0.0);
      float lw2 = max(lw, dot(wider, vec3(0.2126, 0.7152, 0.0722)));
      float compact = (1.0 - smoothstep(tune_fx_area0, tune_fx_area1, lw / max(lf, 0.02))) *
                      (1.0 - smoothstep(tune_fx_wash0, tune_fx_wash1, lw2));
      sum += fx * smoothstep(0.02, 0.08, fm) * compact * fx_bloom * tune_fx_bloom_gain;
    }
  }
  color = vec4(sum, 1.0);
}
