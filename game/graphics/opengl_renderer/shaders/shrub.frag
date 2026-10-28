#version 410 core

layout (location = 0) out vec4 color;
layout (location = 1) out vec4 enh_normal_out;  // Enhanced: world normal, a = 0 for none
in vec3 enh_normal;
in vec3 enh_pos;  // world position (relative to the camera where known)
flat in int enh_lod_kind;  // Enhanced: 1 the game's proto, 2 its detailed model (EnhancedScenery)
uniform vec2 enh_lod;      // the crossfade between them, distance from the camera (0, 0 = off)
uniform float enh_time;    // seconds (the dither moves each frame, TAA blends it smooth)
uniform vec3 enh_model_avg;  // the detailed model's texture, averaged (its far look: the proto's flat one)
// Enhanced: a detailed model's copy swapping to its next simpler version (x: how far through,
// y: +1 the incoming one, -1 the outgoing one, 0 no swap). The two draw complementary pixels of
// one noise, so together they're whole: no hole, no double, and the TAA blends the grain away
flat in vec2 enh_xfade;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;
uniform float enh_foliage;  // Enhanced: 1 on grass and leaves (cutout draws)
uniform float enh_wet;      // Enhanced: how soaked they are (rain), 0..1

// the smooth normal from the vertices where it stays within ~35 degrees of the triangle's own
// face (curved surfaces), else the face's (hard edges a shared strip vertex would round off)
vec4 enh_pack_normal(vec3 n) {
  vec3 f = cross(dFdx(enh_pos), dFdy(enh_pos));
  float flen = length(f);
  float len = length(n);
  if (flen > 0.0) {
    f /= flen;
    if (len > 0.1) {
      n /= len;
      float d = dot(n, f);
      // (blended, not switched: a hard switch drew the lights' highlights as sharp patches)
      n = normalize(mix(f, n * sign(d), smoothstep(0.6, 0.85, abs(d))));
    } else {
      n = f;
    }
  } else if (len > 0.1) {
    n /= len;
  } else {
    return vec4(0.0);
  }
  return vec4(n * 0.5 + 0.5, 1.0);
}

// a 4x4 ordered dither threshold for the pixel, shifted each frame
float enh_dither() {
  ivec2 p = ivec2(gl_FragCoord.xy) & 3;
  int m[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
  return fract((float(m[p.y * 4 + p.x]) + 0.5) / 16.0 + floor(enh_time * 60.0) * 0.618034);
}

void main() {
  // Enhanced: the detailed model near the camera, the game's proto beyond, dithered across the
  // band (each pixel shows one or the other)
  if (enh_lod_kind != 0 && enh_lod.y > 0.0) {
    float far = smoothstep(enh_lod.x, enh_lod.y, length(enh_pos));
    if ((far > enh_dither()) != (enh_lod_kind == 1)) {
      discard;
    }
  }
  if (enh_xfade.y != 0.0) {
    // interleaved gradient noise moved each frame by the golden ratio: spread like blue noise,
    // no grid to see (the 4x4 ordered dither showed one)
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) +
                    floor(enh_time * 60.0) * 0.618034);
    if ((n < enh_xfade.x) != (enh_xfade.y > 0.0)) {
      discard;
    }
  }
  enh_normal_out = enh_pack_normal(enh_normal);
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    if (enh_lod_kind == 2 && enh_lod.y > 0.0) {
      // the model's light and dark streaks fade out toward the far end, where the game's evenly
      // colored proto takes over
      T0.rgb = mix(T0.rgb, enh_model_avg, 0.6 * smoothstep(enh_lod.y * 0.4, enh_lod.y, length(enh_pos)));
    }
    color = fragment_color * T0;
  } else {
    color = fragment_color;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }

  // Enhanced: soaked in the rain: grass and leaves darker and deeper in color (dry straw turns
  // brown-green), stones and props darker (the post pass adds their wet sheen)
  if (enh_wet > 0.0) {
    float w = enh_wet * (enh_foliage > 0.5 ? 1.0 : 0.7);
    color.xyz = mix(color.xyz, color.xyz * color.xyz * 1.8, 0.5 * w) * (1.0 - 0.22 * w);
  }

  color.xyz = mix(color.xyz, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
