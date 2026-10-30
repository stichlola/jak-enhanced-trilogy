#version 410 core

layout (location = 0) out vec4 color;
layout (location = 1) out vec4 enh_normal_out;  // Enhanced: world normal, a = 0 for none
in vec4 vtx_color;
in vec2 vtx_st;
in float fog;

uniform sampler2D tex_T0;

uniform vec4 fog_color;
uniform int ignore_alpha;
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;

uniform int decal_enable;

uniform int gfx_hack_no_tex;

in vec3 vtx_nrm;
in vec3 vtx_key;
in vec3 vtx_view;
in vec3 vtx_key_color;
in vec3 enh_ambient;
in vec3 enh_pos;
uniform mat3 enh_world_to_view;
uniform vec2 enh_rim_spec;  // Enhanced: rim light, highlight strength (0 = vanilla)
uniform float enh_wet;      // Enhanced: rain on the characters (0 = dry)
uniform int enh_cloth;      // Enhanced: see-through cloth, shown as the game draws it (no post)
uniform float enh_char;     // Enhanced: strength of the relief and materials (0 = off, as without them)
uniform int enh_mat;        // Enhanced: the part's material (EnhancedMaterials.h; 0 = as without it)
uniform float enh_eye_lid;  // Enhanced: an eye's eyelid reaches down to this v of its texture (0 = open)

const int MAT_SKIN = 1;
const int MAT_HAIR = 2;
const int MAT_METAL = 3;
const int MAT_LEATHER = 4;
const int MAT_CLOTH = 5;
const int MAT_FUR = 6;
const int MAT_EYE = 7;
const int MAT_LENS = 8;
// the relief's height per material: faces have lines painted on them, glass has none
const float BUMP_K[9] = float[9](0.0, 0.6, 1.0, 0.7, 1.0, 1.0, 1.0, 0.0, 0.0);

void main() {
  // (here, ahead of the discard: derivatives aren't defined for the pixels it drops)
  vec2 st_dx = dFdx(vtx_st);
  vec2 st_dy = dFdy(vtx_st);
  vec3 p_dx = -dFdx(enh_pos);
  vec3 p_dy = -dFdy(enh_pos);

  vec4 T0 = vec4(1.0);
  if (gfx_hack_no_tex == 0) {
    T0 = texture(tex_T0, vtx_st);
    // all merc is tcc=rgba and modulate
    if (decal_enable == 0) {
      color = vtx_color * T0 * 2;
    } else {
      color = T0;
    }
    color.a *= 2;
  } else {
    color.rgb = vtx_color.rgb;

    if (decal_enable == 0) {
      color.a = vtx_color.a * 2;
    } else {
      color.a = 1;
    }
  }

  if (light_dir1_fade_en.w > 0) {
    color.a = light_dir0_fade.w;
  } else if (light_dir1_fade_en.w < 0) {
    color.a *= light_dir0_fade.w;
  }


  if (ignore_alpha == 0 && color.w < 0.128) {
    discard;
  }

  // Enhanced: relief from the texture's brightness, the height 1.2 cm at its most (Mikkelsen's
  // derivative bump). World space, tilting the smooth normal by bump_g; each of the three heights is
  // its own fetch, so a pixel's slope isn't shared with its 2x2 block
  vec3 bump_g = vec3(0.0);
  float nrm_len = length(vtx_nrm);
  if (enh_char > 0.0 && enh_mat > 0 && gfx_hack_no_tex == 0 && decal_enable == 0 && nrm_len > 0.1) {
    // (none far off, where it would only shimmer: 8 to 25 m. None at a cutout's edge either: its
    // see-through texels hold any color)
    float k = enh_char * BUMP_K[enh_mat] * (1.0 - smoothstep(32768.0, 102400.0, length(enh_pos))) *
              (ignore_alpha != 0 ? 1.0 : smoothstep(0.064, 0.2, T0.a));
    if (k > 0.001) {
      const vec3 lum = vec3(0.3, 0.59, 0.11);
      vec3 N = transpose(enh_world_to_view) * (vtx_nrm / nrm_len);
      vec3 r1 = cross(p_dy, N);
      vec3 r2 = cross(N, p_dx);
      float det = dot(p_dx, r1);
      if (abs(det) > 0.01) {
        float h0 = dot(T0.rgb, lum);
        float hx = dot(textureGrad(tex_T0, vtx_st + st_dx, st_dx, st_dy).rgb, lum);
        float hy = dot(textureGrad(tex_T0, vtx_st + st_dy, st_dx, st_dy).rgb, lum);
        // (1.2 cm = 49 game units; the positions are in game units too, so this is a plain slope)
        bump_g = ((hx - h0) * r1 + (hy - h0) * r2) * 49.0 * k / det;
        bump_g -= N * dot(bump_g, N);
        // (held back: at grazing angles a pixel spans so much that the slope explodes)
        float g_len = length(bump_g);
        if (g_len > 0.6) {
          bump_g *= 0.6 / g_len;
        }
      }
    }
  }
  // the bumped normal in the view space the lights are in
  vec3 nb = normalize(vtx_nrm / nrm_len - enh_world_to_view * bump_g);

  // Enhanced: the normal for the post pass (world), as the levels write it: the smooth one (view
  // space here) where it stays near the triangle's own face, else the face's
  {
    vec3 f = cross(p_dx, p_dy);
    float flen = length(f);
    vec3 sn = transpose(enh_world_to_view) * vtx_nrm;
    float len = length(sn);
    vec3 n = vec3(0.0);
    if (flen > 0.0) {
      f /= flen;
      float d = len > 0.1 ? dot(sn / len, f) : 0.0;
      // (blended, not switched: a hard switch drew the lights' highlights as sharp patches)
      n = len > 0.1 ? normalize(mix(f, sn / len * sign(d), smoothstep(0.6, 0.85, abs(d)))) : f;
    }
    // (0.45 long: a character, so the composite spares it the land's late-afternoon deepening and
    // the sun relight's dark side; the game lights characters by its own lights)
    // (cloth is one face, seen from both sides: its normal always toward the camera, from behind too)
    if (enh_cloth != 0 && flen > 0.0 && dot(n, transpose(enh_world_to_view) * normalize(vtx_view)) < 0.0) {
      n = -n;
    }
    // (with the relief. It's raised along the model's outside: n may be the other way, see sign(d))
    if (flen > 0.0 && bump_g != vec3(0.0)) {
      n = normalize(n - bump_g * (dot(n, sn) < 0.0 ? -1.0 : 1.0));
    }
    // (0.52: see-through cloth, the composite puts the game's own color back over it)
    enh_normal_out = flen > 0.0 ? vec4(n * (enh_cloth != 0 ? 0.52 : 0.45) * 0.5 + 0.5, 1.0) : vec4(0.0);
  }

  // Enhanced: the relief in the key light, where the surface isn't already turned away from it
  if (bump_g != vec3(0.0) && length(vtx_key) > 0.01) {
    vec3 n = normalize(vtx_nrm);
    vec3 l = normalize(vtx_key);
    float key_lum = min(dot(clamp(vtx_key_color, 0.0, 1.5), vec3(0.3, 0.59, 0.11)) * 1.5, 1.0);
    color.rgb *= 1.0 + clamp(dot(nb, l) - dot(n, l), -0.35, 0.35) * 0.9 * key_lum *
                           smoothstep(-0.4, 0.2, dot(n, l));
  }

  // Enhanced: a rim of the key light around the silhouette, brighter on the lit side, and a soft
  // highlight (Blinn) from it. The relief goes into the highlight and the soft light past the
  // terminator; the rims keep the smooth normal, they draw the silhouette and would only sparkle
  if (decal_enable == 0 && (enh_rim_spec.x > 0.0 || enh_rim_spec.y > 0.0) && length(vtx_key) > 0.01) {
    vec3 n = normalize(vtx_nrm);
    vec3 v = normalize(vtx_view);
    vec3 l = normalize(vtx_key);
    float ndl = max(dot(n, l), 0.0);
    float rim = pow(1.0 - clamp(abs(dot(n, v)), 0.0, 1.0), 3.0) * (0.35 + 0.65 * ndl);
    float ndl_b = max(dot(nb, l), 0.0);
    // what each material makes of the highlight and of the soft light below
    float c = clamp(enh_char, 0.0, 1.0);
    float sp_pow = 40.0, sp_k = 1.0, wrap_k = 1.0;
    vec3 sp_col = 0.3 + 0.7 * color.rgb;
    vec3 wrap_tint = vec3(1.0, 0.55, 0.4);
    vec3 albedo = clamp(T0.rgb * 2.0, 0.0, 1.0);
    if (c > 0.0) {
      if (enh_mat == MAT_SKIN) {  // soft and broad, and the light scatters further under it
        sp_pow = mix(40.0, 14.0, c);
        sp_k = mix(1.0, 0.55, c);
        wrap_k = mix(1.0, 1.9, c);
        wrap_tint = mix(wrap_tint, vec3(1.0, 0.45, 0.3), c);
      } else if (enh_mat == MAT_HAIR) {  // the strands' own highlight comes below
        sp_k = mix(1.0, 0.25, c);
      } else if (enh_mat == MAT_METAL) {  // tight, bright, the metal's own color
        sp_pow = mix(40.0, 90.0, c);
        sp_k = mix(1.0, 1.7, c);
        sp_col = mix(sp_col, albedo * 1.3 + 0.05, c);
        wrap_k = mix(1.0, 0.5, c);
      } else if (enh_mat == MAT_LEATHER) {
        sp_pow = mix(40.0, 26.0, c);
        sp_k = mix(1.0, 0.8, c);
        wrap_k = mix(1.0, 0.8, c);
      } else if (enh_mat == MAT_CLOTH) {
        sp_pow = mix(40.0, 12.0, c);
        sp_k = mix(1.0, 0.15, c);
        wrap_k = mix(1.0, 1.1, c);
      } else if (enh_mat == MAT_FUR) {
        sp_pow = mix(40.0, 10.0, c);
        sp_k = mix(1.0, 0.1, c);
        wrap_k = mix(1.0, 1.25, c);
      } else if (enh_mat == MAT_LENS) {  // glass: a small hard glint of pure light
        sp_pow = mix(40.0, 220.0, c);
        sp_k = mix(1.0, 2.2, c);
        sp_col = mix(sp_col, vec3(1.0), c);
        wrap_k = mix(1.0, 0.3, c);
      } else if (enh_mat == MAT_EYE) {
        sp_pow = mix(40.0, 90.0, c);
        sp_k = mix(1.0, 0.5, c);
        sp_col = mix(sp_col, vec3(1.0), c);
        wrap_k = mix(1.0, 0.5, c);
      }
    }
    float spec = pow(max(dot(nb, normalize(l + v)), 0.0), sp_pow) * ndl_b;
    vec3 key = clamp(vtx_key_color, 0.0, 1.5);
    color.rgb += key * (rim * enh_rim_spec.x * 0.5 + spec * enh_rim_spec.y * sp_k * sp_col);
    // soft light past the terminator, warm like light scattered under skin and cloth (the game's
    // own light stops hard where the surface turns away)
    float wrap = clamp((dot(nb, l) + 0.45) / 1.45, 0.0, 1.0) - ndl_b;
    color.rgb += color.rgb * key * wrap * wrap_tint * 0.35 * enh_rim_spec.x * wrap_k;
    // and the shadow side catches the sky: a cool rim from the ambient light
    float back = pow(1.0 - clamp(abs(dot(n, v)), 0.0, 1.0), 2.5) * (1.0 - ndl);
    color.rgb += clamp(enh_ambient, 0.0, 1.0) * back * 0.35 * enh_rim_spec.x;

    // and what only some materials have
    if (c > 0.0 && enh_mat > 0) {
      float nv = clamp(abs(dot(nb, v)), 0.0, 1.0);
      vec3 amb = clamp(enh_ambient, 0.0, 1.0);
      // a sky to reflect: dark below the horizon, toward the key light's color above. (the ambient
      // light and key color are all this shader knows of what's around)
      vec3 sky_r = reflect(-v, nb);
      float sky_up = dot(sky_r, enh_world_to_view * vec3(0.0, 1.0, 0.0));
      vec3 sky = mix(amb * 0.5, mix(amb, key, 0.6) * 1.2, smoothstep(-0.2, 0.6, sky_up));
      float wrapped = clamp((dot(nb, l) + 0.5) / 1.5, 0.0, 1.0);
      if (enh_mat == MAT_HAIR) {
        // Kajiya-Kay: a band across the strands, which run along the texture's v (the tangent from
        // its derivative, in the view space). Two lobes: the light's own, and a broader one colored
        // by the hair, shifted toward the root. Where v has no direction here, the plain highlight
        vec3 r1 = enh_world_to_view * cross(p_dy, transpose(enh_world_to_view) * nb);
        vec3 r2 = enh_world_to_view * cross(transpose(enh_world_to_view) * nb, p_dx);
        vec3 t = r1 * st_dx.y + r2 * st_dy.y;
        float t_len = length(t);
        if (t_len > 0.0) {
          t /= t_len;
          vec3 h = normalize(l + v);
          float th1 = dot(normalize(t + nb * 0.12), h);
          float th2 = dot(normalize(t - nb * 0.2), h);
          float strand = pow(max(1.0 - th1 * th1, 0.0), 60.0) * 0.8;
          vec3 strand2 = (0.4 + 0.6 * albedo) * pow(max(1.0 - th2 * th2, 0.0), 14.0) * 0.45;
          color.rgb += key * wrapped * (strand + strand2) * enh_rim_spec.y * c;
        }
      } else if (enh_mat == MAT_METAL) {
        float f = 0.3 + 0.7 * pow(1.0 - nv, 3.0);
        color.rgb = color.rgb * (1.0 - 0.15 * c) + sky * (albedo * 0.85 + 0.1) * f * 0.6 * enh_rim_spec.y * 2.0 * c;
      } else if (enh_mat == MAT_LEATHER) {
        color.rgb += sky * (albedo * 0.5 + 0.1) * pow(1.0 - nv, 3.0) * 0.5 * c;
      } else if (enh_mat == MAT_CLOTH || enh_mat == MAT_FUR) {
        // the fibers along the edge catch the light from behind them
        float sheen = pow(1.0 - nv, enh_mat == MAT_FUR ? 2.0 : 3.0) * (0.3 + 0.7 * wrapped);
        color.rgb += key * (0.25 + 0.75 * albedo) * sheen * (enh_mat == MAT_FUR ? 0.5 : 0.3) * enh_rim_spec.x * c;
      } else if (enh_mat == MAT_LENS) {
        float f = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
        color.rgb += sky * (0.25 + f) * 0.8 * c;
      }
    }
  }

  // Enhanced: wet in the rain: darker cloth and skin, a tight bright highlight from the key light
  // and the grey sky caught on what faces up and at grazing angles
  if (decal_enable == 0 && enh_wet > 0.0 && length(vtx_key) > 0.01) {
    vec3 n = nb;
    vec3 v = normalize(vtx_view);
    vec3 l = normalize(vtx_key);
    float ndl = max(dot(n, l), 0.0);
    float fres = pow(1.0 - clamp(abs(dot(n, v)), 0.0, 1.0), 4.0);
    color.rgb *= 1.0 - 0.22 * enh_wet;
    float spec = pow(max(dot(n, normalize(l + v)), 0.0), 120.0) * ndl;
    color.rgb += clamp(vtx_key_color, 0.0, 1.5) * spec * 0.9 * enh_wet;
    color.rgb += (clamp(enh_ambient, 0.0, 1.0) * 0.6 + 0.06) * fres * 0.5 * enh_wet;
  }

  // Enhanced: live eyes. The eye's mesh spans its whole texture, u and v 0..1 (the EyeRenderer's
  // picture, or the eye's own), v downward and u the same way on both eyes, so one spot in the UVs
  // catches the same light in both. Below the eyelid only: the lid is painted into the texture and
  // closes over all this when he blinks
  if (enh_char > 0.0 && enh_mat == MAT_EYE && length(vtx_key) > 0.01) {
    float c = clamp(enh_char, 0.0, 1.0);
    vec2 e = vtx_st - 0.5;
    float px = max(length(st_dx), length(st_dy));
    float open = smoothstep(enh_eye_lid, enh_eye_lid + 0.06, vtx_st.y);
    // depth: darker toward the eye's corners and in the shade of the upper lid
    float depth = (1.0 - 0.35 * smoothstep(0.3, 0.72, length(e))) *
                  (1.0 - 0.3 * (1.0 - smoothstep(0.0, 0.18, vtx_st.y - enh_eye_lid)));
    color.rgb *= mix(1.0, depth, c * open);
    vec3 key = clamp(vtx_key_color, 0.0, 1.5);
    vec3 amb = clamp(enh_ambient, 0.0, 1.0);
    vec3 n = normalize(vtx_nrm);
    vec3 v = normalize(vtx_view);
    // the sky faintly on the eye's wet surface
    float up = dot(reflect(-v, n), enh_world_to_view * vec3(0.0, 1.0, 0.0));
    vec3 sky = mix(amb * 0.5, mix(amb, key, 0.5), smoothstep(-0.2, 0.6, up));
    color.rgb += sky * pow(1.0 - clamp(abs(dot(n, v)), 0.0, 1.0), 3.0) * 0.35 * c * open;
    // the catchlight, upper right on the iris. Smaller than a pixel from afar: fainter, not blinking
    const float r = 0.07;
    float soft = max(0.02, px);
    float spot = (1.0 - smoothstep(r - soft, r + soft, length(vtx_st - vec2(0.6, 0.36)))) *
                 min(1.0, (r / max(px, 0.0001)) * (r / max(px, 0.0001)));
    color.rgb += (key * 1.2 + amb * 0.25) * spot * c * open;
  }

   color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
}
