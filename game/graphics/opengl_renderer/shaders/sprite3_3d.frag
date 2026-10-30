#version 410 core

out vec4 color;

flat in vec4 fragment_color;
in vec3 tex_coord;
flat in uvec2 tex_info;
flat in int enh_kind;      // Enhanced: 0 untouched, 1 smoke, 2 a blended flame, 3 an added flame
in vec2 enh_q;             // where in the sprite: -1..1 along the screen, not turning with it
flat in vec4 enh_cen;      // the sprite's place in the noise; w: its size (game units)

uniform sampler2D tex_T0;
uniform float alpha_min;
uniform float alpha_max;
uniform sampler3D enh_noise;
uniform vec2 enh_fx;       // the clock (s), how much of this (fx_detail)
uniform int enh_fx_debug;  // JAK_ENHANCED_FX_DEBUG: paints the classified sprites

// a smoke or flame sprite that evolves: the picture bent by a noise drifting through it, and its
// soft parts eaten away by that noise (more as the puff fades), so it isn't a still image turning.
// The mean stays about the same: no brighter or thinner than the game's sprite
void enhanced_sprite() {
  bool flame = enh_kind >= 2;
  float t = enh_fx.x * (flame ? 0.7 : 0.13);
  vec3 dom = vec3(enh_q * 0.22, 0.0) + enh_cen.xyz;
  // (scrolling along the screen's y: up in the flames' case, unless the camera is on its side)
  vec2 flow = texture(enh_noise, dom + vec3(0.0, -t, t * 0.31)).rg - 0.5;
  float fine = texture(enh_noise, dom * 2.7 + vec3(flow * 0.25, 0.0) + vec3(0.0, -t * 1.9, t * 0.57)).r - 0.5;
  float e = flow.x + fine * 0.7;

  vec2 st = tex_coord.xy;
  vec2 from_edge = min(st, 1.0 - st);  // (nothing pulled in from past the picture's edge)
  st += flow * (flame ? 0.22 : 0.12) * enh_fx.y * smoothstep(0.0, 0.18, min(from_edge.x, from_edge.y));
  vec4 T0 = texture(tex_T0, st);
  if (tex_info.y == 0) {
    T0.w = 1.0;
  }
  float shape = tex_info.y == 0 ? dot(T0.rgb, vec3(0.333)) : T0.w;
  float faded = 1.0 - clamp(fragment_color.a, 0.0, 1.0);
  float k = enh_fx.y * ((flame ? 2.0 : 1.5) + faded * 1.2);
  float eaten = max(1.0 + e * k * (1.4 - shape), 0.0);

  color = fragment_color * T0;
  if (enh_kind == 3) {
    color.rgb *= eaten;  // (added: the color is what shows, some of them ignore the alpha)
  } else {
    color.a *= eaten;
  }
  // hotter and cooler parts in a flame, thicker and thinner in smoke
  color.rgb *= 1.0 + e * (flame ? 0.6 : 0.25) * enh_fx.y;

  if (enh_fx_debug != 0) {
    color.rgb = flame ? vec3(1.0, 0.1, 0.05) : vec3(0.1, 1.0, 0.2);
  }
}

void main() {
  if (enh_kind != 0) {
    enhanced_sprite();
  } else {
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    if (tex_info.y == 0) {
      T0.w = 1.0;
    }
    color = fragment_color * T0;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }
}
