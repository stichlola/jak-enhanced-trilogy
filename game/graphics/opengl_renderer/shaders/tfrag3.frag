#version 410 core

layout (location = 0) out vec4 color;
layout (location = 1) out vec4 enh_normal_out;  // Enhanced: world normal, a = 0 for none
in vec3 enh_normal;
in vec3 enh_pos;  // world position (relative to the camera where known)

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;


// the smooth normal from the vertices where it stays within ~35 degrees of the triangle's own
// face (curved surfaces), else the face's (hard edges a shared strip vertex would round off)
uniform int enh_material;  // Enhanced: 0 plain, 1 metal, 2 glossy, 3 sand, 4 rock, 5 rocky ground, 6 brick or stone wall (see EnhancedMaterials.h)

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
  // the material in the 2-bit alpha: 1 plain, 2/3 metal, 1/3 glossy (0: no normal); sand (plain)
  // as a normal 0.9 long (the composite draws the wind's ripples on it)
  float a = enh_material == 1 ? 2.0 / 3.0 : (enh_material == 2 ? 1.0 / 3.0 : 1.0);
  float tag_len = enh_material == 3 ? 0.9 : (enh_material == 4 ? 0.75 : (enh_material == 5 ? 0.6 : 1.0));
  return vec4(n * tag_len * 0.5 + 0.5, a);
}

// Enhanced: the relief near the camera, per draw (EnhancedMaterials.h): rock and brick or stone
// walls only, never a cutout, a blended draw or a decal
uniform float enh_parallax;  // the relief's height, game units (0 = off)

float enh_luma(vec3 c) {
  return dot(c, vec3(0.3, 0.59, 0.11));
}

// parallax occlusion: the view ray marched through the texture's brightness taken as height,
// centered on the polygon so the picture on average stays put (the polygons' edges and the seams
// between them hardly move). Measured against the area around (a coarser mip): a dark brick isn't
// a pit, a stain isn't a hole. Returns the shift in uv; depth stays the polygon's. The post's
// "Surface relief" only shades from the color's slope and its rock_detail stays on the polygon: this
// moves the picture, it adds no light of its own
vec2 enh_parallax_shift(vec2 uv, vec2 dx, vec2 dy, vec3 px, vec3 py) {
  float near = 1.0 - smoothstep(4.0 * 4096.0, 20.0 * 4096.0, length(enh_pos));
  vec2 ts = vec2(textureSize(tex_T0, 0));
  float lod = 0.5 * log2(max(max(dot(dx * ts, dx * ts), dot(dy * ts, dy * ts)), 1e-8));
  near *= 1.0 - smoothstep(4.0, 6.0, lod);  // (under the pixel's size: nothing to see, save the reads)
  vec3 n = cross(px, py);
  float det = length(n);
  if (near <= 0.0 || det < 1e-6) {
    return vec2(0.0);
  }
  n /= det;
  // uv's gradient along the surface (no tangents in the vertices), so the height is real distance
  // however the texture is stretched
  vec3 r1 = cross(py, n), r2 = cross(n, px);
  vec3 gu = (r1 * dx.x + r2 * dy.x) / det, gv = (r1 * dx.y + r2 * dy.y) / det;
  vec3 ray = normalize(enh_pos);
  float rn = dot(ray, n);
  vec3 side = ray - n * rn;
  float graze = 1.0 - abs(rn);
  // (the grazing limit: past it the picture would smear)
  vec2 shift = vec2(dot(gu, side), dot(gv, side)) / max(abs(rn), 0.3) * enh_parallax * near;
  float hlod = max(lod + 0.7, 0.0);  // (a little blurred: the texels' noise isn't relief)
  float avg = enh_luma(textureLod(tex_T0, uv, hlod + 3.0).rgb);
  int steps = int(mix(4.0, 8.0, near * graze));
  float dt = 1.0 / float(steps);
  float t = 1.0;
  float gap0 = 1.0;
  for (int i = 0; i <= steps; i++) {
    float ti = float(i) * dt;
    float h = enh_luma(textureLod(tex_T0, uv + shift * (ti - 0.5), hlod).rgb);
    float gap = 1.0 - ti - clamp(0.5 + (h - avg) * 2.0, 0.0, 1.0);
    if (gap <= 0.0) {
      // (between the last two steps, where the two lines cross)
      t = max(ti + dt * gap / (gap0 - gap), 0.0);
      break;
    }
    gap0 = gap;
  }
  return shift * (t - 0.5);
}

void main() {
  enh_normal_out = enh_pack_normal(enh_normal);
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    vec4 T0;
    if (enh_parallax > 0.0) {
      // (the derivatives out here, before any branch on the pixel; the mip stays the polygon's, a
      // step in the shift would otherwise show as a seam)
      vec2 uv = tex_coord.xy, dx = dFdx(uv), dy = dFdy(uv);
      vec3 px = dFdx(enh_pos), py = dFdy(enh_pos);
      T0 = textureGrad(tex_T0, uv + enh_parallax_shift(uv, dx, dy, px, py), dx, dy);
    } else {
      T0 = texture(tex_T0, tex_coord.xy);
    }
    color = fragment_color * T0;
  } else {
    color = fragment_color/2;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }

  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
