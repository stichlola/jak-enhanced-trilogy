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
uniform sampler2D enh_depth;  // the scene's depth, copied before the sprites draw
uniform vec2 enh_depth_ab;    // ndc z = a + b / distance (b = 0: not known)
uniform vec4 enh_sun;      // toward the sun: along the screen's x and y, toward the camera; w: strength
uniform vec2 enh_rise;     // world up along the screen's x and y
uniform vec2 enh_fx;       // the clock (s), how much of this (fx_detail)
uniform int enh_fx_debug;  // JAK_ENHANCED_FX_DEBUG: paints the classified sprites

// a smoke or flame sprite that evolves: the picture bent by a noise drifting through it, and its
// soft parts eaten away by that noise (more as the puff fades), so it isn't a still image turning.
// The mean stays about the same: no brighter or thinner than the game's sprite
void enhanced_sprite() {
  bool flame = enh_kind >= 2;
  float t = enh_fx.x * (flame ? 0.7 : 0.13);
  vec3 dom = vec3(enh_q * 0.22, 0.0) + enh_cen.xyz;
  // (the noise climbs through the sprite, whichever way the camera leans)
  vec2 flow = texture(enh_noise, dom + vec3(-enh_rise * t, t * 0.31)).rg - 0.5;
  float fine = texture(enh_noise, dom * 2.7 + vec3(flow * 0.25 - enh_rise * t * 1.9, t * 0.57)).r - 0.5;
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

  // smoke is lit by the sun as a ball would be: a normal from where in the sprite this is (bumped by
  // the same noise), a bit of light always getting through
  if (!flame) {
    vec3 n = normalize(vec3(enh_q + flow * 1.5, 0.75));
    float lit = dot(n, enh_sun.xyz) * 0.5 + 0.5;
    color.rgb *= mix(1.0, 0.7 + 0.52 * lit, enh_sun.w * enh_fx.y);
  }

  // soft: it fades where it meets a wall or the ground, over a third of its size, instead of being
  // cut along a hard line
  if (enh_depth_ab.y != 0.0) {
    float sd = texelFetch(enh_depth, ivec2(gl_FragCoord.xy), 0).r;
    if (sd > 1e-7) {  // (0: the sky)
      float scene = abs(enh_depth_ab.y / (sd * 2.0 - 1.0 - enh_depth_ab.x));
      float own = abs(enh_depth_ab.y / (gl_FragCoord.z * 2.0 - 1.0 - enh_depth_ab.x));
      float reach = clamp(enh_cen.w * 0.35, 0.25 * 4096.0, 3.0 * 4096.0);
      float soft = smoothstep(0.0, 1.0, (scene - own) / reach);
      soft = mix(1.0, soft, min(enh_fx.y * 2.0, 1.0));
      if (enh_kind == 3) {
        color.rgb *= soft;
      } else {
        color.a *= soft;
      }
    }
  }

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
