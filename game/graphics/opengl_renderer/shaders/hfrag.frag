#version 410 core

out vec4 color;

in vec4 fragment_color;
in vec2 tex_coord;
in float fogginess;
in vec2 world_xz;
uniform sampler2D tex_T0;
uniform sampler2D tex_T1;   // Enhanced: seamless sand detail (when enhanced_sand is set)
uniform int enhanced_sand;
uniform float sand_scale;
uniform vec3 sand_mean;     // average color of tex_T1
uniform float sand_detail;  // how much of its detail goes over the game's sand
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;


void main() {
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, tex_coord);
    if (enhanced_sand != 0) {
      // the game's own sand keeps its colors; the seamless picture only adds its grain, at two
      // scales and turned against each other so neither repeat shows
      vec3 a = texture(tex_T1, world_xz * sand_scale).rgb;
      vec3 b = texture(tex_T1, mat2(0.8, -0.6, 0.6, 0.8) * world_xz * sand_scale * 2.7).rgb;
      vec3 grain = clamp((a * 0.55 + b * 0.45) / max(sand_mean, vec3(1e-3)), 0.6, 1.5);
      T0.rgb *= mix(vec3(1.0), grain, sand_detail);
    }
    color = fragment_color * T0 * 2;
  } else {
    color = fragment_color;
  }

   color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
