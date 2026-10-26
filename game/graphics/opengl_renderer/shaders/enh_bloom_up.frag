#version 410 core

// Jak Enhanced Trilogy: bloom upsample, 3x3 tent filter, added onto the bigger level by blending.

uniform sampler2D tex_T0;
uniform vec2 texel;  // source texel size
uniform float radius;

in vec2 tex_coord;
out vec4 color;

void main() {
  vec2 d = texel * radius;
  vec2 uv = tex_coord;
  vec3 sum = texture(tex_T0, uv).rgb * 4.0;
  sum += (texture(tex_T0, uv + vec2(-d.x, 0)).rgb + texture(tex_T0, uv + vec2(d.x, 0)).rgb +
          texture(tex_T0, uv + vec2(0, -d.y)).rgb + texture(tex_T0, uv + vec2(0, d.y)).rgb) * 2.0;
  sum += texture(tex_T0, uv + vec2(-d.x, -d.y)).rgb + texture(tex_T0, uv + vec2(d.x, -d.y)).rgb +
         texture(tex_T0, uv + vec2(-d.x, d.y)).rgb + texture(tex_T0, uv + vec2(d.x, d.y)).rgb;
  color = vec4(sum / 16.0, 1.0);
}
