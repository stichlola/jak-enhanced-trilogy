#version 410 core

// Enhanced: dense grass (EnhancedGrass), in the manner of Ghost of Tsushima's: no meshes, every
// blade is made here from the instance and vertex numbers. The fields are cut into tiles; each
// tile drawn has a level of detail (how many blades, how many segments each). The blades of a
// tile are the first ones of one evenly spread sequence (R2), so a lower level is a subset of a
// higher one: across a level's edge the extra blades shrink away (and the rest widen), nothing
// pops. A blade: a curved strip, wide at the foot, pointed, leaning and bending in the wind's
// travelling gusts, pushed aside by Jak; shrub.frag colors it like the game's grass.

layout (location = 0) in vec4 tile;  // its corner x, z (game units), its level of detail, -

uniform vec4 hvdf_offset;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform sampler1D tex_T10;  // the level's time of day colors
uniform sampler2D tex_T3;   // the field's floor heights (r, game units; far below: no floor), how lush (g, 0..1)

uniform int enh_blades;         // blades per tile at this level of detail
uniform int enh_segs;           // segments per blade (2 segs + 1 vertices)
uniform float enh_tile;         // tile side (game units)
uniform vec4 enh_field;         // the heights' corner x, z, their side (game units), the water's height
uniform vec4 enh_zone;          // the field's middle x, z, its radius (game units), density 0..1
uniform vec3 enh_counts;        // blades per tile at the three levels
uniform vec3 enh_reach;         // how far each level reaches (game units)
uniform int enh_tod;            // a time of day color of the game's grass there
uniform vec3 enh_base_rgb;      // and its own color
uniform float enh_time;
uniform float enh_wind;
uniform vec4 enh_player;        // Jak (game units), the push's radius
uniform vec4 enh_pushers[8];    // the others near him (enemies, passers-by, vehicles), w: radius
uniform int enh_pusher_count;
uniform sampler2D tex_T4;       // the crush map (EnhancedScenery): where the grass still lies
uniform vec4 enh_crush;         // its corner (world x, z), its size (game units), on
uniform vec4 enh_style;         // the painted grass's look: its height, its blades' width, the wind's bend (times the usual)
uniform vec3 enh_grass_color;   // its green
uniform vec3 enh_grass_dry;     // its drier tips
uniform float tune_grass_ground_sat = 1.2;  // (tuning: the floor color's saturation for the blades)
uniform vec4 enh_grass_ground;  // ground-class fields: a tint over the floor's color (rgb), w: how much the blades take each spot's color (the field's b, packed)
uniform float enh_screen;            // ground-class fields: the frame so far copied (1: blades take the floor's color as drawn under their foot)
uniform sampler2D enh_scene_color;   // its color, mipmapped
uniform sampler2D enh_scene_depth;   // its depth
uniform sampler2D enh_scene_normal;  // its normals (their length: the post's material tag)
uniform float tune_grass_scr_gain = 1.4;  // (tuning: the blades' tips over the floor's color: their mean as bright as the floor, 0.95-1.0)
uniform float tune_grass_scr_lod = 2.5;   // (tuning: how blurred the floor's color is taken, mip level)
uniform float tune_grass_scr_tol = 1.0;   // (tuning: meters along the view the foot's floor may be off)
uniform float tune_grass_scr_foot = 0.45;  // (tuning: the blades' feet, darker in the grass around)
uniform float tune_grass_scr_sat = 0.85;  // (tuning: the floor color's saturation in the blades, 1: as drawn; the post reddens orange blades)
uniform float tune_grass_ground_up = 0.0; // (tuning: the blades' normals toward the floor's, 1: lit as the floor)
uniform float tune_grass_scr_dbg = 0.0;  // (tuning: 1 green = floor seen, red = hidden; 2 the floor's color alone)

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
out vec3 enh_normal;
out vec3 enh_pos;
flat out int enh_lod_kind;
flat out vec2 enh_xfade;

uint hash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

float rnd(uint a, uint b) {
  return float(hash(a * 0x9E3779B1u ^ hash(b + 0x632BE5ABu)) >> 8) * (1.0 / 16777216.0);
}

// smooth value noise (patches)
float vnoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  uvec2 c = uvec2(ivec2(i) + 32768);
  float a = rnd(c.x, c.y), b = rnd(c.x + 1u, c.y);
  float d = rnd(c.x, c.y + 1u), e = rnd(c.x + 1u, c.y + 1u);
  return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}

float floor_at(vec2 xz) {
  return texture(tex_T3, (xz - enh_field.xy) / enh_field.z).r;
}

void hide() {
  gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  fragment_color = vec4(0.0);
  tex_coord = vec3(0.0);
  fogginess = 0.0;
  enh_normal = vec3(0.0, 1.0, 0.0);
  enh_pos = vec3(0.0);
  enh_lod_kind = 0;
  enh_xfade = vec2(0.0);
}

vec4 to_clip(vec3 world) {
  vec3 vert = world - cam_trans.xyz;
  vec4 p = -pc_camera[3];
  p -= pc_camera[0] * vert.x;
  p -= pc_camera[1] * vert.y;
  p -= pc_camera[2] * vert.z;
  p.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  return p;
}

void main() {
  const float m = 4096.0;
  int b = gl_InstanceID % enh_blades;
  uvec2 tid = uvec2(ivec2(floor(tile.xy / enh_tile)) + 100000);
  uint seed = hash(tid.x * 7919u + tid.y);

  // where: the R2 sequence from the tile's own start (its first blades evenly spread)
  vec2 start = vec2(rnd(seed, 1u), rnd(seed, 2u));
  vec2 uv = fract(start + float(b) * vec2(0.7548776662, 0.5698402910));
  vec2 xz = tile.xy + uv * enh_tile;
  uint bs = hash(seed ^ uint(b) * 0x27d4eb2du);

  // the floor: none, under the water, or too steep: no blade
  float y = floor_at(xz);
  float e = 0.35 * m;
  float yx = floor_at(xz + vec2(e, 0.0)), yz = floor_at(xz + vec2(0.0, e));
  if (y < -1e6 || yx < -1e6 || yz < -1e6 || y < enh_field.w + 0.12 * m ||
      length(vec2(yx - y, yz - y)) / e > 0.75) {
    hide();
    return;
  }

  // how much grass here: as lush as the field says (painted, or thinning toward its edge), with a
  // ragged border, and in patches
  vec2 wm = xz / m;
  float lush = texture(tex_T3, (xz - enh_field.xy) / enh_field.z).g + (vnoise(wm / 9.0) - 0.5) * 0.3;
  float patchy = vnoise(wm / 4.0 + 17.0);
  float dense = smoothstep(0.05, 0.4, lush) * smoothstep(0.0, 0.25, patchy + 0.2) * enh_zone.w;
  if (rnd(bs, 3u) > dense) {
    hide();
    return;
  }

  // the levels of detail: this blade's reach (by its place in the sequence), shrinking out over
  // the last part of it; the fewer blades farther, the wider each
  vec3 cam = cam_trans.xyz;
  float d = length(vec3(xz.x, y, xz.y) - cam);
  float reach = float(b) < enh_counts.z ? enh_reach.z : float(b) < enh_counts.y ? enh_reach.y : enh_reach.x;
  float keep = 1.0 - smoothstep(reach * 0.75, reach, d);
  if (keep <= 0.0) {
    hide();
    return;
  }
  // (as many blades shrink away at an edge as the rest widen by: the cover the same across it, no
  // ring where the denser grass ends)
  float widen = mix(1.0, enh_counts.x / enh_counts.y, smoothstep(enh_reach.x * 0.75, enh_reach.x, d)) *
                mix(1.0, enh_counts.y / enh_counts.z, smoothstep(enh_reach.y * 0.75, enh_reach.y, d));

  // the blade: tall where it's lush, shorter toward the edge
  float h = mix(0.55, 1.25, rnd(bs, 4u)) * mix(0.6, 1.2, patchy) * mix(0.35, 1.15, smoothstep(0.1, 0.9, lush)) *
            keep * m * enh_style.x;
  float w = mix(0.045, 0.075, rnd(bs, 5u)) * widen * m * enh_style.y;
  float yaw = 6.2831853 * rnd(bs, 6u);
  vec2 face = vec2(cos(yaw), sin(yaw));  // the blade's flat side faces this way
  // (turned a little toward the camera: edge on, a blade is a hairline)
  vec2 to_cam = normalize(cam.xz - xz + 1e-3);
  face = normalize(mix(face, to_cam * sign(dot(face, to_cam) + 1e-4), 0.35));
  vec2 side = vec2(-face.y, face.x);

  // its lean: its own, the wind's gusts rolling across the field, a flutter; pushed by Jak
  vec2 wind_dir = normalize(vec2(0.8, 0.6));
  float gust = 0.5 + 0.5 * sin(dot(wm, wind_dir) / 1.6 - enh_time * 2.1 + vnoise(wm / 6.0) * 4.0);
  float flutter = sin(enh_time * 5.3 + rnd(bs, 7u) * 6.2832) * 0.08;
  vec2 lean = face * (rnd(bs, 8u) - 0.5) * 0.8 + wind_dir * (gust * gust * 0.7 + flutter) * enh_wind * enh_style.z;
  if (enh_player.w > 0.0) {
    // (tall grass: parted over a wider ring than the tufts' push)
    for (int i = -1; i < enh_pusher_count; i++) {
      vec4 pusher = i < 0 ? enh_player : enh_pushers[i];
      vec2 away = xz - pusher.xz;
      float near = (1.0 - smoothstep(pusher.w * 0.5, pusher.w * 1.6, length(away))) *
                   (1.0 - smoothstep(1.0 * m, 2.5 * m, abs(y - pusher.y)));
      lean += normalize(away + 1e-3) * near * 1.6;
    }
    // and lying where someone went through a little while ago, rising back over a few seconds
    if (enh_crush.w > 0.0) {
      vec2 cuv = (xz - enh_crush.xy) / enh_crush.z;
      if (all(greaterThan(cuv, vec2(0.0))) && all(lessThan(cuv, vec2(1.0)))) {
        vec3 cr = texture(tex_T4, cuv).xyz;
        float lie = length(cr.xy) * (1.0 - smoothstep(1.5, 3.0, abs(cr.z - y / m)));
        lean += cr.xy / max(length(cr.xy), 1e-4) * lie * 1.4;
      }
    }
  }
  float bend = min(length(lean), 1.4);
  vec2 lean_dir = bend > 1e-3 ? lean / length(lean) : face;

  // the vertex: segment row t along the blade, left or right (the tip: one point)
  int row = gl_VertexID / 2;
  float t = float(min(row, enh_segs)) / float(enh_segs);
  float s = row >= enh_segs ? 0.0 : (gl_VertexID % 2 == 0 ? -1.0 : 1.0);
  // (a quadratic arc: rising, bending over toward the lean; shorter as it bends)
  float rise = h * t * (1.0 - 0.35 * bend * t);
  vec2 over = lean_dir * h * 0.6 * bend * t * t;
  float width = w * pow(1.0 - t, 0.75);
  vec3 world = vec3(xz.x + over.x + side.x * s * width * 0.5, y + rise - 0.02 * m,
                    xz.y + over.y + side.y * s * width * 0.5);
  // its normal: the flat side, tipped back as it bends, and rounded across (lit like a blade, not a
  // card)
  vec3 n = normalize(vec3(face.x, 0.0, face.y) + vec3(0.0, 0.9 * bend * t + 0.25, 0.0) -
                     vec3(lean_dir.x, 0.0, lean_dir.y) * 0.3 * bend);
  n = normalize(n + vec3(side.x, 0.0, side.y) * s * 0.45);

  vec3 vert = world - cam;
  vec4 transformed = -pc_camera[3];
  transformed -= pc_camera[0] * vert.x;
  transformed -= pc_camera[1] * vert.y;
  transformed -= pc_camera[2] * vert.z;
  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;

  // the color: deep at the foot (the grass around shades it), fresh green up the blade, patches
  // of drier yellow-green, each blade its own shade
  // (a deep, saturated green: the composite turns the desert's olive and yellow-greens to sand,
  // a strong green it leaves alone; dry yellow-green tips in some patches)
  // (toned down from the painted green toward a natural, slightly warm one: it read as highlighter)
  vec3 green = mix(vec3(dot(enh_grass_color, vec3(0.3, 0.59, 0.11))), enh_grass_color, 0.6) * vec3(1.05, 0.88, 0.8);
  vec3 dry = mix(vec3(dot(enh_grass_dry, vec3(0.3, 0.59, 0.11))), enh_grass_dry, 0.7) * vec3(1.0, 0.92, 0.85);
  if (enh_grass_ground.w > 0.0) {
    // (the floor's own color under the blade, as drawn: the blades come out its tone)
    ivec2 sz = textureSize(tex_T3, 0);
    ivec2 ti = clamp(ivec2(floor((xz - enh_field.xy) / enh_field.z * vec2(sz))), ivec2(0), sz - 1);
    float pk = texelFetch(tex_T3, ti, 0).b;
    if (pk > 0.5) {
      uint u = uint(pk + 0.5);
      vec3 c = vec3(float(u & 255u), float((u >> 8u) & 255u), float((u >> 16u) & 255u)) / 127.5;
      c = max(mix(vec3(dot(c, vec3(0.3, 0.59, 0.11))), c, tune_grass_ground_sat), 0.0) * enh_grass_ground.rgb;
      green = mix(green, c, enh_grass_ground.w);
      dry = mix(dry, min(c * 1.25 + 0.04, vec3(2.0)), enh_grass_ground.w);
    }
  }  vec3 tipc = mix(green, dry, smoothstep(0.45, 0.85, vnoise(wm / 7.0 + 3.0)) * 0.6 * t);
  tipc *= mix(0.75, 1.2, rnd(bs, 9u));
  vec3 albedo = mix(tipc * 0.35, tipc, pow(t, 0.6));
  vec4 tod = texelFetch(tex_T10, enh_tod, 0);
  // (the host tree's colors give the hour's light, not their own tint: a dry desert shrub's would
  // make the grass straw)
  float base = max(dot(enh_base_rgb, vec3(0.2126, 0.7152, 0.0722)), 0.05);
  fragment_color = vec4(albedo * 2.2 * vec3(base) * tod.rgb * 4.0, 0.5);

  float ground_seen = 0.0, ground_tag = 0.0;
  if (enh_screen > 0.0) {
    // the floor as drawn under the blade's foot (the frame before the grass): where it's in view
    // and not hidden behind something nearer (its depth within a meter or so of the foot's along
    // the view), the blade takes its color
    vec3 foot = vec3(xz.x, y, xz.y);
    vec3 dir = normalize(foot - cam) * tune_grass_scr_tol * m;
    vec4 c0 = to_clip(foot);
    vec4 cn = to_clip(foot - dir), cf = to_clip(foot + dir);
    float seen = 0.0;
    vec3 floor_c = vec3(0.0);
    float floor_tag = 0.0;
    if (c0.w > 0.0 && cn.w > 0.0) {
      vec2 suv = c0.xy / c0.w * 0.5 + 0.5;
      // (a foot just off the screen's edge (the near blades at its bottom): the floor at the edge)
      vec2 inside = clamp(suv, vec2(0.002), vec2(0.998));
      float off = length(suv - inside);
      if (off < 0.12) {
        float ds = texture(enh_scene_depth, inside).r;
        float dn = cn.z / cn.w * 0.5 + 0.5, df = cf.z / cf.w * 0.5 + 0.5;
        float lo = min(dn, df), hi = max(dn, df);
        float pad = (hi - lo) * 0.25;
        seen = off > 0.0 ? 1.0 - smoothstep(0.06, 0.12, off)
                         : smoothstep(lo - pad, lo, ds) * (1.0 - smoothstep(hi, hi + pad, ds));
        suv = inside;
        floor_c = textureLod(enh_scene_color, suv, tune_grass_scr_lod).rgb;
        floor_tag = length(texture(enh_scene_normal, suv).rgb * 2.0 - 1.0);
      }
    }
    ground_seen = seen;
    ground_tag = floor_tag;
    if (seen > 0.0) {
      // (shrub.frag tones foliage down, 0.62 of its saturation and 0.92 of it: undone here, so the
      // blades come out the floor's own tone)
      float l = dot(floor_c, vec3(0.3, 0.59, 0.11));
      vec3 pre = max(vec3(l) + (floor_c - vec3(l)) * tune_grass_scr_sat / 0.62, vec3(0.0)) / 0.92;
      vec3 tip = pre * tune_grass_scr_gain * mix(0.8, 1.15, rnd(bs, 9u)) *
                 mix(1.0, 1.12, smoothstep(0.45, 0.85, vnoise(wm / 7.0 + 3.0)) * t);
      vec3 sc = mix(tip * tune_grass_scr_foot, tip, pow(t, 0.6));
      fragment_color.rgb = mix(fragment_color.rgb, sc, seen * min(enh_screen, 1.0));
    }
    // (lit by the post like the floor they stand on: their normals toward the floor's)
    vec3 fn = normalize(vec3(-(yx - y) / e, 1.0, -(yz - y) / e));
    n = normalize(mix(n, fn, tune_grass_ground_up));
    if (tune_grass_scr_dbg > 1.5) {
      fragment_color.rgb = floor_c;
    } else if (tune_grass_scr_dbg > 0.5) {
      fragment_color.rgb = vec3(1.0 - seen, seen, 0.0) * 0.5;
    }
  }

  tex_coord = vec3(0.5, 0.5, 1.0);
  enh_normal = n;
  enh_pos = vert;
  enh_lod_kind = 0;
  enh_xfade = vec2(0.0);
  if (ground_seen > 0.5) {
    enh_xfade = vec2(ground_tag, 0.0);  // (shrub.frag: the floor's material tag for these blades)
  }
}
