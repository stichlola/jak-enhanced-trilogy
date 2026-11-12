#version 410 core

layout (location = 0) out vec4 color;
layout (location = 1) out vec4 enh_normal_out;  // Enhanced: world normal, a = 0 for none
layout (location = 2) out vec4 enh_albedo_out;  // Enhanced: texture color before the baked light, a: that light / 2 (0: none)
in vec3 enh_normal;
in vec3 enh_pos;  // world position (relative to the camera where known)
in float enh_leaf;  // Enhanced: a TIE tree's leaves (tfrag3.vert)

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
uniform int enh_material;  // Enhanced: 0 plain, 1 metal, 2 glossy, 3 sand, 4 rock, 5 rocky ground, 6 wall, 7 wood, 8 rusty metal, 9 painted metal, 10 tile, 11 window, 12 lamp, 13 facade (see EnhancedMaterials.h)

// Enhanced: true PBR (EnhancedPbr.h): the HD texture's companion map on unit 11, R G the tangent-space
// normal (x along u, y up the picture: v runs down it), B the roughness about the class's own (0.5)
uniform sampler2D tex_T11;
uniform int enh_pbr;  // 1: the draw's texture has one (0: none, or PBR off)
vec4 g_pbr = vec4(0.5, 0.5, 0.5, 0.0);
bool g_pbr_on = false;
// per material class (enh_material, 0..13: plain, metal, glossy, sand, rock, rocky ground, wall, wood,
// rusty, painted, tile, window, lamp, facade): how much of the map's relief, the roughness, and how
// far the map's roughness moves it
const float PBR_K[14] = float[14](0.8, 0.4, 0.15, 0.65, 1.3, 1.1, 1.0, 0.9, 0.9, 0.35, 0.35, 0.0, 0.0, 0.0);
const float PBR_R[14] = float[14](0.7, 0.35, 0.12, 0.8, 0.5, 0.72, 0.68, 0.62, 0.78, 0.4, 0.25, 0.08, 0.6, 0.6);
const float PBR_RR[14] = float[14](0.5, 0.5, 0.2, 0.4, 0.6, 0.5, 0.5, 0.5, 0.4, 0.4, 0.3, 0.1, 0.2, 0.2);

// the map's relief the same near and far: its mips average the bumps away (the desert rock maps' RMS
// slope: 0.28 at mip 0, 0.18 at 2, 0.11 at 3, 0.055 at 4, a fifth by ~40 m), so close up the
// full-size cracks shaded dark while the same rock farther off was a flat pale yellow, the look
// changing as you walk. Close up a little softer, and farther part of what the mips lost given back
// (the coarser bumps still in the mip, scaled up). The mip as the anisotropic filter picks it (x16)
uniform float tune_pbr_even = 1.0;   // 0: off (the map's own mips, as before)
uniform float tune_pbr_near = 0.5;  // the relief at full size, times the class's
uniform float tune_pbr_keep = 0.6;   // of the slope the mips lost, this power given back (1: all)
float enh_pbr_gain(vec2 duv1, vec2 duv2) {
  if (tune_pbr_even <= 0.0 || (tune_pbr_even > 1.5 && gl_FragCoord.x < 640.0)) {
    return 1.0;  // (2: test only, A/B in one frame: the right half on; assumes a 1280 wide window)
  }
  vec2 ts = vec2(textureSize(tex_T11, 0));
  vec2 a = duv1 * ts, b = duv2 * ts;
  float big = max(dot(a, a), dot(b, b)), small = min(dot(a, a), dot(b, b));
  float lod = 0.5 * log2(max(max(small, big / 256.0), 1e-12));
  // (the slope's RMS over the mips, measured: ~flat to mip 1, then x0.6 a mip)
  float lost = exp2(0.75 * clamp(lod - 1.0, 0.0, 5.0));
  return tune_pbr_near * pow(lost, tune_pbr_keep);
}

// the map's normal on the surface: the level has no tangents, so the texture's axes come from the
// screen-space derivatives of position and uv (a cotangent frame, Schuler 2013). n is either side:
// the frame flips with it
vec3 enh_pbr_bump(vec3 n, vec3 dp1, vec3 dp2, vec2 duv1, vec2 duv2) {
  int mi = clamp(enh_material, 0, 13);
  float k = PBR_K[mi];
  if (!g_pbr_on || k <= 0.0) {
    return n;
  }
  k *= enh_pbr_gain(duv1, duv2);
  vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
  vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
  vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
  float tb = max(dot(t, t), dot(b, b));
  if (!(tb > 1e-30)) {
    return n;
  }
  float inv = inversesqrt(tb);
  vec2 m = (g_pbr.xy * 2.0 - 1.0) * k;
  m *= min(1.0, 0.95 * inversesqrt(max(dot(m, m), 1e-8)));  // (a boosted far slope: never past ~70 degrees)
  float mz = sqrt(max(1.0 - dot(m, m), 0.05));
  return normalize(t * inv * m.x - b * inv * m.y + n * mz);
}

vec3 g_dp1, g_dp2;
vec2 g_duv1, g_duv2;

// Enhanced: the detail material (EnhancedDetail.h, detail.json) on unit 12: a tileable picture laid
// in world space (x along X, y along Z), R G its normal, B a roughness offset about 0.5, A AO. The
// game's texture keeps the color; this adds the fine relief (grains, small ripples) near the camera
uniform sampler2D tex_T12;
uniform int enh_detail;      // 0 none, 1 on, 2 on and the albedo may carry its roughness (PBR on)
uniform vec4 cam_trans;      // (tfrag3.vert's: enh_pos is relative to it; also enh_on_sand below)
uniform vec4 enh_detail_k;   // 1 / tile (game units), normal, roughness, AO strengths
uniform float tune_detail = 1.0;                  // shaders.tune_detail: strength, 0 off
uniform vec2 tune_detail_fade = vec2(5.0, 45.0);  // m: full up to, gone by
vec4 g_det = vec4(0.0, 0.0, 0.5, 1.0);  // x y: slopes along world X and Z, z roughness offset, w AO
float g_det_k = 0.0;  // its strength at this pixel (0: none)

float enh_detail_noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  vec2 h = vec2(127.1, 311.7);
  float a = fract(sin(dot(i, h)) * 43758.5453);
  float b = fract(sin(dot(i + vec2(1.0, 0.0), h)) * 43758.5453);
  float c = fract(sin(dot(i + vec2(0.0, 1.0), h)) * 43758.5453);
  float d = fract(sin(dot(i + vec2(1.0, 1.0), h)) * 43758.5453);
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// the tile and a copy 2.7x as big turned 37 degrees, their weights varied by a slow noise, so
// neither repeat shows; the slopes (not the normals) summed. Mips average the normal flat with
// distance; the fade by distance only saves the reads. (The camera's part wrapped first: precision)
void enh_detail_sample() {
  if (enh_detail == 0 || tune_detail <= 0.0) {
    return;
  }
  float k = 1.0 - smoothstep(tune_detail_fade.x * 4096.0, tune_detail_fade.y * 4096.0, length(enh_pos));
  if (k <= 0.0) {
    return;
  }
  const mat2 rot = mat2(0.8, -0.6, 0.6, 0.8);
  float s1 = enh_detail_k.x, s2 = enh_detail_k.x / 2.7;
  vec2 uv1 = fract(cam_trans.xz * s1) + enh_pos.xz * s1;
  vec2 uv2 = fract((rot * cam_trans.xz) * s2 + 0.37) + (rot * enh_pos.xz) * s2;
  vec4 a = textureGrad(tex_T12, uv1, g_dp1.xz * s1, g_dp2.xz * s1);
  vec4 b = textureGrad(tex_T12, uv2, (rot * g_dp1.xz) * s2, (rot * g_dp2.xz) * s2);
  float nz = enh_detail_noise((cam_trans.xz + enh_pos.xz) / (4096.0 * 9.0));
  float wa = mix(0.55, 1.0, nz), wb = mix(0.75, 0.35, nz);
  vec2 na = a.xy * 2.0 - 1.0, nb = b.xy * 2.0 - 1.0;
  vec2 ga = na / max(sqrt(1.0 - dot(na, na)), 0.3);
  vec2 gb = nb / max(sqrt(1.0 - dot(nb, nb)), 0.3);
  // (b's slopes back from its turned frame to the world's: rot's transpose)
  vec2 g = ga * wa + (gb * rot) * wb;
  g_det = vec4(g, 0.5 + (a.b - 0.5) * wa + (b.b - 0.5) * wb, mix(1.0, a.a, wa) * mix(1.0, b.a, wb));
  g_det_k = k * tune_detail;
}

// the detail's slopes on the surface's normal (UDN: added along the world's X and Z projected onto
// the surface). n is either side (the composite flips it toward the camera): the slopes flip with it.
// Only on what faces up (sand on a steep face would stretch)
vec3 enh_detail_bump(vec3 n) {
  float k = g_det_k * enh_detail_k.y * smoothstep(0.35, 0.7, abs(n.y));
  if (k <= 0.0) {
    return n;
  }
  vec3 t = normalize(vec3(1.0, 0.0, 0.0) - n * n.x);
  vec3 b = normalize(vec3(0.0, 0.0, 1.0) - n * n.z);
  float side = n.y < 0.0 ? -1.0 : 1.0;
  return normalize(n + (t * g_det.x + b * g_det.y) * (k * side));
}

// Enhanced: rock lying on the dunes. The desert's mounds end in flat skirts flush with the sand,
// painted and lit to match it (the game shows no edge), but tagged rock the composite lit them as
// rock (lifted, warmed, the rock picture) next to the sand's darker grade: a pale hard-edged polygon
// round every mound. The dunes' heights (Hfrag.cpp, drawn first) tell how far a rock pixel stands
// over the sand: on it (and facing up) it gets the sand's light, through the length of its tag (0.75
// toward 0.82, enh_composite.frag's on_sand), fading out over its first half meter up.
// shaders.tune_rock_on_sand 0: off (every rock pixel tagged rock, as before)
uniform int enh_ground_on;     // 1: tex_T19 has the dunes' heights this frame
uniform sampler2D tex_T19;     // their 512x512 grid, 8 m apart from the world's origin, 1e30 where none
// (cam_trans: declared above, with the detail material)
uniform float tune_rock_on_sand = 1.0;  // (2: shows the weight, red rock to green sand)
uniform vec2 tune_rock_sand_h = vec2(0.15, 0.6);    // m over the dunes: all sand, none
uniform vec2 tune_rock_sand_up = vec2(0.6, 0.85);   // facing up (world normal's y): none, all
uniform float enh_ground_light;  // the dunes' time of day colors' mean brightness now (Hfrag.cpp)
uniform vec2 tune_rock_sand_lit = vec2(0.5, 0.75);   // its own baked light over the dunes': none, all

float g_t0_luma = -1.0;  // the texture's brightness here (set before enh_pack_normal)
float enh_on_sand(vec3 face_n) {
  if (enh_ground_on == 0 || tune_rock_on_sand <= 0.0) {
    return 0.0;
  }
  vec3 w = enh_pos + cam_trans.xyz;
  vec2 g = w.xz / 32768.0;
  ivec2 i0 = ivec2(floor(g));
  if (i0.x < 0 || i0.y < 0 || i0.x > 510 || i0.y > 510) {
    return 0.0;
  }
  vec2 fr = g - vec2(i0);
  float h00 = texelFetch(tex_T19, i0, 0).r;
  float h10 = texelFetch(tex_T19, i0 + ivec2(1, 0), 0).r;
  float h01 = texelFetch(tex_T19, i0 + ivec2(0, 1), 0).r;
  float h11 = texelFetch(tex_T19, i0 + ivec2(1, 1), 0).r;
  if (max(max(h00, h10), max(h01, h11)) > 1e29) {
    return 0.0;  // (no dune there)
  }
  float h = mix(mix(h00, h10, fr.x), mix(h01, h11, fr.x), fr.y);
  float above = (w.y - h) / 4096.0;  // m
  float on = 1.0 - smoothstep(tune_rock_sand_h.x, tune_rock_sand_h.y, above);
  // (and painted as pale as the sand: the patches' rims, not their dark middles, which the
  // composite's rock light lifts on purpose)
  // (fragment_color is 2x: 1 = lit as the dunes; the rims ~0.7-1.1, the patches' dark middles ~0.45-0.52)
  float lit = 0.5 * dot(fragment_color.rgb, vec3(0.3, 0.59, 0.11)) / max(enh_ground_light, 0.02);
  return on * smoothstep(tune_rock_sand_up.x, tune_rock_sand_up.y, abs(face_n.y)) *
         smoothstep(tune_rock_sand_lit.x, tune_rock_sand_lit.y, lit);
}

vec4 enh_pack_normal(vec3 n) {
  vec3 f = cross(g_dp1, g_dp2);
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
  n = enh_pbr_bump(n, g_dp1, g_dp2, g_duv1, g_duv2);
  // (the detail material's fine relief over the map's broad one)
  n = enh_detail_bump(n);
  // the material in the 2-bit alpha: 1 plain, 2/3 metal, 1/3 glossy (0: no normal); sand (plain)
  // as a normal 0.9 long (the composite draws the wind's ripples on it)
  // (the rougher classes a step under 1.0, still plain scenery to the composite's tests)
  float a = enh_material == 1 || enh_material == 8 || enh_material == 9 ? 2.0 / 3.0
            : (enh_material == 2 || enh_material == 10 || enh_material == 11 ? 1.0 / 3.0 : 1.0);
  float tag_len = enh_material == 3 ? 0.9 : (enh_material == 4 ? 0.75 : (enh_material == 5 ? 0.6 : 1.0));
  if (enh_material >= 6) {
    tag_len = enh_material == 6 || enh_material == 9 || enh_material == 11 ? 0.99 : 0.995;
  }
  // (rock on the sand: 0.75 + up to 0.07, read back as a weight; see enh_on_sand)
  if (enh_material == 4 && flen > 0.0) {
    tag_len = 0.75 + 0.07 * enh_on_sand(f);
  }
  // (lamps, 12: plain, 0.2 long, under every other tag; the composite lights them at night)
  if (enh_material == 12) {
    tag_len = 0.2;
  }
  // (far buildings' low-res walls, 13: plain, 0.1 long; the composite lights windows on them at night)
  if (enh_material == 13) {
    tag_len = 0.1;
  }
  // (a TIE tree's leaves, plain: 0.675 long, a spare length the composite maps back to plain right
  // after reading it; its backlight lights them through toward the sun, as the shrubs' 0.97)
  if (enh_leaf > 0.5 && tag_len == 1.0 && a == 1.0) {
    tag_len = 0.675;
  }
  return vec4(n * tag_len * 0.5 + 0.5, a);
}

// Enhanced: the relief near the camera, per draw (EnhancedMaterials.h): rock and brick or stone
// walls only, never a cutout, a blended draw or a decal
uniform float enh_parallax;  // the relief's height, game units (0 = off)

// the walls' repeats broken up where they show (the composite's notile() can't: it only has the
// finished picture, the texture's own repeat is gone by then). A slow noise over the picture picks
// one of 8 offsets of it, two neighbouring ones blended where they differ least. The offsets in
// quarters of a tile, so mortar lines and courses mostly stay in line; only along the axes where
// the texture does repeat, per draw (EnhancedMaterials.h)
uniform vec2 enh_notile;

float enh_notile_noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
  float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
  float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
  float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

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

// Enhanced: the detailed model stands in near the camera, this proto beyond, dithered across the
// band: the same pixels as shrub.frag leaves the model (EnhancedScenery)
flat in int enh_lod_kind;
uniform vec2 enh_lod;  // distance from the camera (0, 0 = off)
float enh_dither() {
  ivec2 p = ivec2(gl_FragCoord.xy) & 3;
  int m[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
  return fract((float(m[p.y * 4 + p.x]) + 0.5) / 16.0);
}

uniform float enh_fade_in = 1.0;  // a level just drawn: 0 -> 1 over half a second (no pop-in)

void main() {
  enh_albedo_out = vec4(0.0);
  // (the derivatives first, in uniform control flow: the normal and the PBR map need them)
  g_dp1 = dFdx(enh_pos);
  g_dp2 = dFdy(enh_pos);
  g_duv1 = dFdx(tex_coord.xy);
  g_duv2 = dFdy(tex_coord.xy);
  enh_detail_sample();
  if (enh_fade_in < 1.0 && enh_fade_in <= enh_dither()) {
    discard;
  }
  if (enh_lod_kind != 0 && enh_lod.y > 0.0 &&
      smoothstep(enh_lod.x, enh_lod.y, length(enh_pos)) <= enh_dither()) {
    discard;
  }
  vec2 uv_used = tex_coord.xy;  // (where the picture was read: the PBR map is read there too)
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    vec4 T0;
    if (enh_parallax > 0.0 || enh_notile != vec2(0.0)) {
      // (the derivatives out here, before any branch on the pixel; the mip stays the polygon's, a
      // step in the shift would otherwise show as a seam)
      vec2 uv = tex_coord.xy, dx = dFdx(uv), dy = dFdy(uv);
      vec3 px = dFdx(enh_pos), py = dFdy(enh_pos);
      // (near only: three reads instead of one; gone by 60 m)
      float nk = enh_notile == vec2(0.0) ? 0.0 : 1.0 - smoothstep(30.0 * 4096.0, 60.0 * 4096.0, length(enh_pos));
      vec2 off_a = vec2(0.0), off_b = vec2(0.0);
      float pick = 0.0;
      if (nk > 0.0) {
        float l = enh_notile_noise(uv * 0.37) * 8.0;
        float ia = floor(l);
        pick = l - ia;
        off_a = floor(sin(vec2(3.0, 7.0) * ia) * 4.0 + 0.5) * 0.25 * enh_notile;
        off_b = floor(sin(vec2(3.0, 7.0) * (ia + 1.0)) * 4.0 + 0.5) * 0.25 * enh_notile;
      }
      // (the relief marched on the offset that shows most, so it matches the picture)
      vec2 shift = enh_parallax > 0.0 ? enh_parallax_shift(uv + (pick < 0.5 ? off_a : off_b), dx, dy, px, py) : vec2(0.0);
      T0 = textureGrad(tex_T0, uv + shift + off_a, dx, dy);
      uv_used = uv + shift + off_a;
      if (nk > 0.0) {
        vec4 b = textureGrad(tex_T0, uv + shift + off_b, dx, dy);
        vec4 mixed = mix(T0, b, smoothstep(0.2, 0.8, pick - 0.1 * dot(T0.rgb - b.rgb, vec3(1.0))));
        T0 = nk >= 1.0 ? mixed : mix(textureGrad(tex_T0, uv + shift, dx, dy), mixed, nk);
      }
    } else {
      T0 = texture(tex_T0, tex_coord.xy);
    }
    color = fragment_color * T0;
    g_t0_luma = dot(T0.rgb, vec3(0.3, 0.59, 0.11));
    float baked = clamp(dot(fragment_color.rgb, vec3(0.3, 0.59, 0.11)) * 0.5, 1.0 / 255.0, 1.0);
    // (a < 1: alpha 1 is the PBR-coded albedo below)
    enh_albedo_out = vec4(T0.rgb, min(baked, 0.6));
    // (the detail material's occlusion: in the hollows between grains and ripples)
    color.rgb *= mix(1.0, g_det.w, clamp(enh_detail_k.w * g_det_k, 0.0, 1.0));
    if (enh_pbr != 0 || (enh_detail == 2 && g_det_k > 0.0)) {
      if (enh_pbr != 0) {
        g_pbr = textureGrad(tex_T11, uv_used, g_duv1, g_duv2);
        g_pbr_on = true;
      }
      // the roughness for the composite, 6 bits in the albedo's spare low bits (10-bit target, 8-bit
      // texture): code 1..63 over its three channels, alpha 1 says it's there (the other level
      // shaders write their light under 2/3). Where the baked light is ~0 (alpha 0: no albedo for the
      // lamps), none: the composite takes the class's own
      int mi = clamp(enh_material, 0, 13);
      float r = clamp(PBR_R[mi] + (g_pbr.b - 0.5) * 2.0 * PBR_RR[mi] +
                      (g_det.z - 0.5) * 2.0 * enh_detail_k.z * g_det_k, 0.03, 1.0);
      int code = 1 + int(r * 62.0 + 0.5);
      vec3 a8 = floor(clamp(T0.rgb, 0.0, 1.0) * 255.0 + 0.5);
      vec3 bits = vec3(float((code >> 4) & 3), float((code >> 2) & 3), float(code & 3));
      enh_albedo_out = vec4((a8 * 4.0 + bits) / 1023.0, baked * 3.0 >= 0.5 ? 1.0 : 0.0);
    }
  } else {
    color = fragment_color/2;
  }
  enh_normal_out = enh_pack_normal(enh_normal);
  if (tune_rock_on_sand > 1.5 && enh_material == 4) {
    // (test: the rock's on-sand weight, red rock to green sand)
    vec3 fn = cross(g_dp1, g_dp2);
    float ws = length(fn) > 0.0 ? enh_on_sand(normalize(fn)) : 0.0;
    color.rgb = mix(vec3(0.5, 0.0, 0.0), vec3(0.0, 0.5, 0.0), ws);
    if (tune_rock_on_sand > 2.5) {
      // (3: its baked light over the dunes', grey: 0.5 = the same)
      color.rgb = vec3(0.25 * dot(fragment_color.rgb, vec3(0.3, 0.59, 0.11)) / max(enh_ground_light, 0.02), g_t0_luma, 0.0);  // (R 0.5 = as lit as the dunes, G its texture)
    }
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }

  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
