"""Rubble brush for the Jak 2 plaza junctions (Blender, piazza_jak2.blend).

A sidebar panel "Macerie" in the 3D view:
- "Crea tela": a grid draped on the ground around both passages (object "Tela macerie", vertex
  group "Macerie"), to paint in Weight Paint where rubble should go: more weight = denser, bigger.
- "Dipingi giunzioni": paints the canvas by itself where the junctions show. From eye height along
  the walkways, rays all around: a downward ray that finds no ground (a hole) or a ray that meets the
  back of a face (the inside of a shell) marks the spot.
- "Piazza macerie": scatters pieces from the template collections ("Pezzi grandi", "Pezzi da
  posizionare") over the painted area as linked copies in "Passaggio/Macerie pennello", adding to
  what is there, then wipes the paint (kept in "Macerie ultima"): each batch only follows its own
  strokes. "Altra variante" redoes the last batch. Each piece follows the ground's slope, is sunk a
  little, may not hover on one edge, go through a wall or another piece, or reach into the
  walkways. "Togli macerie" removes every brush piece.

Load it in the .blend as a text block and run it once (or tick Register). Coordinates: GOAL meters
(x, y up, z) are Blender (x, -z, y). The game cuts the passages out of mhcitya and ctyport
(Shader.cpp, collide-cache.gc): rays ignore those levels inside the same volumes.
"""
import math
from collections import Counter
import random

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree

CANVAS = "Tela macerie"
GROUP = "Macerie"
LAST_GROUP = "Macerie ultima"  # the strokes of the last batch, for "Altra variante"
OUT_COLLECTION = "Macerie pennello"
TEMPLATES = ("Lastre", "Pezzi grandi", "Pezzi da posizionare", "Pezzi piccoli")
# never hit by rays: templates, helpers, collision-only meshes
IGNORED_COLLECTIONS = TEMPLATES + ("Kit pezzi", "Kit macerie (rubblea)", "Collisione nuova", "Pennelli",
                                   "Jak2 ctywide")
CUT_LEVELS = ("Jak3 mhcitya", "Jak3 ctyport")
# canvas region, GOAL x and z ranges: the whole plaza and both passages
REGIONS = (((-20.0, 405.0), (360.0, 1260.0)),)
# walkways, GOAL (x, z) segment + half width: nothing lower than 6 m above the floor may enter them
PATHS = (((24.0, 959.0), (104.0, 959.0), 12.0), ((238.0, 1131.0), (198.0, 1208.0), 8.0),
         # into the north part of the plaza, up to the palace steps
         ((190.0, 900.0), (200.0, 800.0), 6.0), ((200.0, 800.0), (150.0, 765.0), 6.0))
CELL = 2.5


def goal(x, y, z):
    return Vector((x, -z, y))


def in_cut(p):
    """Is the Blender point inside a passage cut (same volumes as the game)?"""
    x, y, z = p.x, p.z, -p.y
    if 42 < x < 92 and 7 < y < 60 and 950 < z < 968:
        return True
    ax, az, bx, bz = 229.0, 1146.0, 200.0, 1206.0
    t = max(0.0, min(1.0, ((x - ax) * (bx - ax) + (z - az) * (bz - az)) / ((bx - ax) ** 2 + (bz - az) ** 2)))
    return math.hypot(x - (ax + (bx - ax) * t), z - (az + (bz - az) * t)) < 6.5 and 4.6 < y < 16


def path_distance(px, pz):
    """Smallest (distance - half width) to a walkway, GOAL x z."""
    best = 1e9
    for (a, b, hw) in PATHS:
        abx, abz = b[0] - a[0], b[1] - a[1]
        t = max(0.0, min(1.0, ((px - a[0]) * abx + (pz - a[1]) * abz) / (abx * abx + abz * abz)))
        best = min(best, math.hypot(px - (a[0] + abx * t), pz - (a[1] + abz * t)) - hw)
    return best


class Scene:
    """Ray casts against what the game draws there."""

    def __init__(self, context, extra_ignore=()):
        self.sc = context.scene
        self.dg = context.evaluated_depsgraph_get()
        self.ignore = set(extra_ignore)
        self.cut_objs = set()
        for o in bpy.data.objects:
            cols = {c.name for c in o.users_collection}
            if (cols & set(IGNORED_COLLECTIONS) or o.get("set_invisible") or o.get("export_skip")
                    or o.name.startswith(("Collisione da togliere", "Cube", "Giunzione"))
                    or not o.visible_get()):
                self.ignore.add(o.name)
            if cols & set(CUT_LEVELS):
                self.cut_objs.add(o.name)

    def ray(self, origin, direction, dist=200.0):
        """(location, normal, object name) of the first drawn surface, or None."""
        o = origin.copy()
        left = dist
        for _ in range(24):
            hit, loc, nrm, _i, ob, _m = self.sc.ray_cast(self.dg, o, direction, distance=left)
            if not hit:
                return None
            if ob.name in self.ignore or (ob.name in self.cut_objs and in_cut(loc)):
                step = (loc - o).length + 0.02
                o = loc + direction * 0.02
                left -= step
                if left <= 0:
                    return None
                continue
            return loc, nrm, ob.name
        return None

    def ground(self, p, above=25.0, depth=60.0):
        return self.ray(p + Vector((0, 0, above)), Vector((0, 0, -1)), above + depth)


# ------------------------------------------------------------------ canvas

def make_canvas(context):
    scene = Scene(context, extra_ignore=(CANVAS,))
    bm = bmesh.new()
    for (x0, x1), (z0, z1) in REGIONS:
        nx, nz = int((x1 - x0) / CELL) + 1, int((z1 - z0) / CELL) + 1
        grid = []
        for i in range(nx):
            col = []
            for j in range(nz):
                p = goal(x0 + i * CELL, 0, z0 + j * CELL)
                g = scene.ground(p + Vector((0, 0, 60)), above=0.0, depth=90.0)
                p.z = (g[0].z if g else 8.0) + 0.12
                col.append(bm.verts.new(p))
            grid.append(col)
        for i in range(nx - 1):
            for j in range(nz - 1):
                bm.faces.new((grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]))
    old = bpy.data.objects.get(CANVAS)
    me = bpy.data.meshes.new(CANVAS)
    bm.to_mesh(me)
    bm.free()
    # keep the paint of an earlier canvas: each new vertex takes the nearest old one's weight
    weights = None
    if old and GROUP in old.vertex_groups:
        old_w = read_weights(old)
        kd = KDTree(len(old.data.vertices))
        for v in old.data.vertices:
            co = old.matrix_world @ v.co
            kd.insert((co.x, co.y, 0.0), v.index)
        kd.balance()
        weights = np.zeros(len(me.vertices), np.float32)
        for v in me.vertices:
            _co, i, d = kd.find((v.co.x, v.co.y, 0.0))
            if i is not None and d < CELL * 1.5:
                weights[v.index] = old_w[i]
    if old:
        bpy.data.objects.remove(old)
    ob = bpy.data.objects.new(CANVAS, me)
    col = bpy.data.collections.get("Pennelli") or bpy.data.collections.new("Pennelli")
    if col.name not in context.scene.collection.children:
        context.scene.collection.children.link(col)
    col.objects.link(ob)
    vg = ob.vertex_groups.new(name=GROUP)
    vg.add(range(len(me.vertices)), 0.0, 'REPLACE')
    if weights is not None:
        write_weights(ob, weights)
    ob.show_in_front = True  # paint it over the buildings
    return ob


def read_weights(ob, group=GROUP):
    if group not in ob.vertex_groups:
        return np.zeros(len(ob.data.vertices), np.float32)
    gi = ob.vertex_groups[group].index
    w = np.zeros(len(ob.data.vertices), np.float32)
    for v in ob.data.vertices:
        for g in v.groups:
            if g.group == gi:
                w[v.index] = g.weight
    return w


def write_weights(ob, w, group=GROUP):
    vg = ob.vertex_groups.get(group) or ob.vertex_groups.new(name=group)
    for x in np.unique(w):
        vg.add(np.nonzero(w == x)[0].tolist(), float(x), 'REPLACE')


# ------------------------------------------------------------------ junction finder

def paint_junctions(context, canvas, paths=None):
    """Weight the canvas where the junctions show from the walkways (all, or the given ones)."""
    scene = Scene(context, extra_ignore={CANVAS} | set(placed_names()))
    verts = np.array([v.co[:] for v in canvas.data.vertices], np.float32)
    verts = verts @ np.array(canvas.matrix_world.to_3x3()).T + np.array(canvas.matrix_world.translation)
    w = read_weights(canvas)
    spots = []
    for (a, b, hw) in (paths or PATHS):
        length = math.hypot(b[0] - a[0], b[1] - a[1])
        for k in range(int(length / 4) + 1):
            t = k * 4 / length
            p = goal(a[0] + (b[0] - a[0]) * t, 0, a[1] + (b[1] - a[1]) * t)
            g = scene.ground(p + Vector((0, 0, 60)), above=0.0, depth=90.0)
            if not g:
                continue
            eye = g[0] + Vector((0, 0, 1.6))
            for az in range(0, 360, 6):
                for el in (-15, -8, 0, 10):
                    d = Vector((math.cos(math.radians(az)) * math.cos(math.radians(el)),
                                math.sin(math.radians(az)) * math.cos(math.radians(el)),
                                math.sin(math.radians(el))))
                    hit = scene.ray(eye, d, 80.0)
                    if hit is None:
                        if el < 0:  # no ground where there should be: a hole
                            spots.append((eye + d * (1.6 / math.sin(math.radians(-el))), 1.0))
                        continue
                    loc, nrm, _name = hit
                    if nrm.dot(d) > 0.05 and (loc - eye).length < 45:  # inside of a shell
                        spots.append((loc, 0.5))
    for loc, strength in spots:
        if path_distance(loc.x, -loc.y) < 1.0:
            continue
        d = np.hypot(verts[:, 0] - loc.x, verts[:, 1] - loc.y)
        near = d < 4.0
        w[near] = np.minimum(1.0, w[near] + strength * 0.35 * (1 - d[near] / 4.0))
    write_weights(canvas, w)
    return len(spots)


# ------------------------------------------------------------------ placement

def placed_names():
    col = bpy.data.collections.get(OUT_COLLECTION)
    return [o.name for o in col.objects] if col else []


def wall_tree(scene, bounds):
    """BVH of the wall triangles drawn around the canvas, world space: steep and at least 1.2 m tall
(curbs, steps and planter edges don't count)."""
    lo, hi = bounds
    verts, tris, normals, centers = [], [], [], []
    for o in bpy.data.objects:
        if o.type != 'MESH' or o.name in scene.ignore or o.name == CANVAS:
            continue
        if any(w in o.name.lower() for w in ("rub", "shard", "debris")):
            continue  # rubble against rubble is fine
        bb = [o.matrix_world @ Vector(c) for c in o.bound_box]
        if (max(v.x for v in bb) < lo.x or min(v.x for v in bb) > hi.x or
                max(v.y for v in bb) < lo.y or min(v.y for v in bb) > hi.y):
            continue
        me = o.data
        co = np.empty(len(me.vertices) * 3, np.float32)
        me.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3) @ np.array(o.matrix_world.to_3x3()).T + np.array(o.matrix_world.translation)
        me.calc_loop_triangles()
        for lt in me.loop_triangles:
            a, b, c = (co[i] for i in lt.vertices)
            if (a[0] < lo.x and b[0] < lo.x and c[0] < lo.x) or (a[0] > hi.x and b[0] > hi.x and c[0] > hi.x):
                continue
            if (a[1] < lo.y and b[1] < lo.y and c[1] < lo.y) or (a[1] > hi.y and b[1] > hi.y and c[1] > hi.y):
                continue
            n = np.cross(b - a, c - a)
            ln = np.linalg.norm(n)
            if ln < 1e-6 or abs(n[2] / ln) > 0.6:
                continue
            if max(a[2], b[2], c[2]) - min(a[2], b[2], c[2]) < 1.2:
                continue
            base = len(verts)
            verts += [Vector(a), Vector(b), Vector(c)]
            tris.append((base, base + 1, base + 2))
            normals.append(Vector((n[0] / ln, n[1] / ln, 0.0)))
            centers.append(Vector(((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, 0.0)))
    return (BVHTree.FromPolygons(verts, tris), normals, centers) if tris else None


def piece_tree(me, m):
    return BVHTree.FromPolygons([m @ v.co for v in me.vertices], [p.vertices[:] for p in me.polygons])


def aabb(me, m):
    pts = np.array([(m @ v.co)[:] for v in me.vertices])
    return pts.min(0), pts.max(0)


def overlap_ratio(a, b):
    lo = np.maximum(a[0], b[0])
    hi = np.minimum(a[1], b[1])
    if np.any(hi <= lo):
        return 0.0
    inter = np.prod(hi - lo)
    va, vb = np.prod(np.maximum(a[1] - a[0], 1e-3)), np.prod(np.maximum(b[1] - b[0], 1e-3))
    return inter / min(va, vb)


def template_shape(o):
    """Local extents of a template, its thin and long axes and its kind: slab, beam or block."""
    co = np.array([v.co[:] for v in o.data.vertices])
    ext = co.max(0) - co.min(0)
    order = np.argsort(ext)  # thin, mid, long
    thin, mid, long_ = ext[order]
    if thin < 0.3 * mid:
        kind = "beam" if long_ > 3.0 * mid else "slab"
    else:
        kind = "block"
    return ext, int(order[0]), int(order[2]), kind


def lie_flat(thin_axis):
    """Rotation turning the template's thin axis up."""
    if thin_axis == 0:
        return Quaternion((0, 1, 0), math.radians(90))
    if thin_axis == 1:
        return Quaternion((1, 0, 0), math.radians(90))
    return Quaternion()


class Obstacles:
    """Placed pieces, tested with their real shape: pieces may touch, not go through each other."""

    def __init__(self, objects):
        self.items = []  # [lo, hi, object, tree or None]
        for o in objects:
            lo, hi = aabb(o.data, o.matrix_world)
            self.items.append([lo, hi, o, None])

    def add(self, ob, box, tree):
        self.items.append([box[0], box[1], ob, tree])

    def hits(self, box, tree):
        n = 0
        for it in self.items:
            if np.any(box[1] < it[0]) or np.any(box[0] > it[1]):
                continue
            if it[3] is None:
                it[3] = piece_tree(it[2].data, it[2].matrix_world)
            n += len(tree.overlap(it[3]))
        return n


def settle(scene, me, m, sink, rejected, lean):
    """Drop a posed piece onto the ground: the median of its lowest points touches it, sunk a
    little. None if it would sit on one edge (a leaning slab instead rests on its low edge)."""
    pts = [m @ vv.co for vv in me.vertices]
    zmin = min(q.z for q in pts)
    h = max(q.z for q in pts) - zmin
    band = [q for q in pts if q.z < zmin + 0.2 * h + 0.05] or pts
    gaps = []
    for q in band[:16]:
        gq = scene.ground(q, above=h + 2.0, depth=h + 10.0)
        if gq:
            gaps.append(q.z - gq[0].z)
    if not gaps:
        rejected["no ground"] += 1
        return None
    gaps.sort()
    dz = -(gaps[0] if lean else gaps[len(gaps) // 2]) - sink * max(h, 0.3)
    if not lean and max(gaps) + dz > 0.3 * h + 0.3:
        rejected["edge"] += 1
        return None
    return Matrix.Translation((0, 0, dz)) @ m


def place(context, canvas, props):
    """Add one batch of pieces over the current paint, then move the paint to LAST_GROUP.

    Style "Crollo" (like Jak 3's fallen city): slabs and beams lie on their flat side, a third of
    the slabs lean up on one edge, and each big piece gets a few small flat bits around it."""
    batch = int(context.scene.get("macerie_batch", 0)) + 1
    rng = random.Random(props.macerie_seed * 1000 + batch)
    out = bpy.data.collections.get(OUT_COLLECTION) or bpy.data.collections.new(OUT_COLLECTION)
    if out.name not in bpy.data.collections["Passaggio"].children:
        bpy.data.collections["Passaggio"].children.link(out)
    scene = Scene(context, extra_ignore={CANVAS})
    crollo = props.macerie_stile == 'CROLLO'
    pool = [o for c in TEMPLATES if c in bpy.data.collections for o in bpy.data.collections[c].objects
            if o.type == 'MESH' and len(o.data.vertices) >= 3]
    shapes = {o.name: template_shape(o) for o in pool}
    templates = [o for o in pool if props.macerie_min <= max(o.dimensions) <= props.macerie_max * 1.2]
    slab_names = ("lastra_", "muro_", "trave_")
    if crollo:  # thick broken slabs and beams, a few of the ripped panels and girders, no spiky rocks
        others = [o for o in templates if not o.name.startswith(slab_names + ("frammento_",))
                  and not any(k in o.name for k in ("rubble-pile", "shard", "rebar", "brick", "crater"))
                  and shapes[o.name][3] != "block"]
        templates = [o for o in templates if o.name.startswith(slab_names)] + \
            [o for o in others if rng.random() < 0.2]
        bits = [o for o in pool if o.name.startswith("frammento_")]
    else:
        bits = [o for o in pool if max(o.dimensions) <= 6.0 and shapes[o.name][3] in ("slab", "beam")]
    if not templates:
        return 0, Counter()
    templates.sort(key=lambda o: max(o.dimensions))
    sizes = [max(o.dimensions) for o in templates]

    mw = canvas.matrix_world
    w = read_weights(canvas)
    co = [mw @ v.co for v in canvas.data.vertices]
    faces = [(p.vertices[:], p.area, float(np.mean(w[list(p.vertices)]))) for p in canvas.data.polygons]
    faces = [f for f in faces if f[2] > 0.02]
    if not faces:
        return 0, Counter()  # nothing painted: keep the last batch's strokes
    lo = Vector((min(c.x for c in co), min(c.y for c in co), 0))
    hi = Vector((max(c.x for c in co), max(c.y for c in co), 0))
    walls = wall_tree(scene, (lo, hi))
    total = sum(a * wt for _v, a, wt in faces) * props.macerie_densita / 100.0
    count = int(total + rng.random())
    obstacles = Obstacles([o for o in bpy.data.collections["Passaggio"].all_objects
                           if o.type == 'MESH' and not o.name.startswith("Passaggio rampa")
                           and o.name != "Riempimento buchi"
                           and (o.name.startswith(("grande_", "nuovo_", "pass")) or o.get("macerie_batch"))])
    made = 0
    rejected = Counter()
    face_weights = [a * fw for _v, a, fw in faces]

    def try_piece(tpl, p, small):
        """Pose, settle and check one piece at p; the new object or None."""
        g = scene.ground(p, above=8.0, depth=30.0)
        if not g:
            rejected["no ground"] += 1
            return None
        gp, gn = g[0], g[1]
        _ext, thin, long_axis, kind = shapes[tpl.name]
        s = rng.uniform(0.85, 1.15)
        up = (gn.normalized() + Vector((0, 0, 1))).normalized()
        yaw = Quaternion((0, 0, 1), rng.uniform(0, 2 * math.pi))
        lean = False
        if crollo and kind != "block":
            base = lie_flat(thin)
            if kind == "slab" and not small and rng.random() < 0.4:
                # lift one long edge: turn about the slab's long axis (once lying flat)
                axis = base @ Vector([1.0 if i == long_axis else 0.0 for i in range(3)])
                tilt = Quaternion(axis, math.radians(rng.uniform(15, 60)) * rng.choice((-1, 1)))
                lean = True
            else:
                tilt = Quaternion(Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), 0)).normalized(),
                                  math.radians(rng.uniform(0, 6)))
            rot = Vector((0, 0, 1)).rotation_difference(up) @ yaw @ tilt @ base
        else:
            tilt_axis = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), 0)).normalized()
            rot = (Quaternion(tilt_axis, math.radians(rng.uniform(0, props.macerie_inclinazione)))
                   @ Vector((0, 0, 1)).rotation_difference(up) @ yaw)
        m = Matrix.Translation(gp) @ rot.to_matrix().to_4x4() @ Matrix.Scale(s, 4)
        me = tpl.data
        m = settle(scene, me, m, 0.02 if (crollo and kind != "block") else props.macerie_affonda,
                   rejected, lean)
        if m is None:
            return None
        if any(path_distance(q.x, -q.y) < 0.5 and q.z < gp.z + 6.0 for q in (m @ vv.co for vv in me.vertices)):
            rejected["walkway"] += 1
            return None
        # through a wall: slide it away from the wall, up to 4.5 m
        if walls:
            tree, wn, wc = walls
            hits = []
            for _push in range(7):
                hits = piece_tree(me, m).overlap(tree)
                if not hits:
                    break
                centre = m.translation
                away = Vector((0, 0, 0))
                for _ip, iw in hits:
                    away += wn[iw] if wn[iw].dot(centre - wc[iw]) >= 0 else -wn[iw]
                if away.length < 1e-6:
                    break
                m = Matrix.Translation(away.normalized() * 0.75) @ m
            if hits:
                rejected["wall"] += 1
                return None
            if any(path_distance(q.x, -q.y) < 0.5 and q.z < gp.z + 6.0
                   for q in (m @ vv.co for vv in me.vertices)):
                rejected["walkway"] += 1
                return None
        box = aabb(me, m)
        tree = piece_tree(me, m)
        # touching is fine (a leaning slab rests on its neighbour), going through is not
        if obstacles.hits(box, tree) > (0 if small else 6):
            rejected["overlap"] += 1
            return None
        ob = bpy.data.objects.new("macerie_" + tpl.name.split("_", 1)[-1], me)
        ob.matrix_world = m
        ob["set_collision"] = 1
        ob["ignore"] = 1
        ob["macerie_batch"] = batch
        out.objects.link(ob)
        obstacles.add(ob, box, tree)
        return ob

    for _ in range(count):
        vs, _a, wt = rng.choices(faces, weights=face_weights)[0]
        for _attempt in range(8):
            u, v = rng.random(), rng.random()
            c0, c1, c2, c3 = (co[i] for i in vs)
            p = (c0 * (1 - u) + c1 * u) * (1 - v) + (c3 * (1 - u) + c2 * u) * v
            if path_distance(p.x, -p.y) < 1.0:
                rejected["walkway"] += 1
                continue
            # bigger pieces where the paint is stronger
            want = props.macerie_min + (props.macerie_max - props.macerie_min) * wt * rng.uniform(0.5, 1.0)
            k = min(range(len(sizes)), key=lambda i: abs(sizes[i] - want))
            tpl = templates[max(0, min(len(templates) - 1, k + rng.randint(-2, 2)))]
            ob = try_piece(tpl, p, False)
            if ob is None:
                continue
            made += 1
            # small flat bits scattered around it
            if crollo and bits:
                r = max(ob.dimensions) * 0.6 + 1.0
                for _b in range(rng.randint(1, 3)):
                    a = rng.uniform(0, 2 * math.pi)
                    q = ob.matrix_world.translation + Vector((math.cos(a), math.sin(a), 0)) * rng.uniform(0.4, 1.0) * r
                    if path_distance(q.x, -q.y) < 1.0:
                        continue
                    if try_piece(rng.choice(bits), q, True) is not None:
                        made += 1
            break
    context.scene["macerie_batch"] = batch
    write_weights(canvas, w, LAST_GROUP)
    write_weights(canvas, np.zeros_like(w))
    return made, rejected


def repaint_from_pieces(canvas):
    """Paint the canvas where the brush pieces stand (to redo them in another style)."""
    col = bpy.data.collections.get(OUT_COLLECTION)
    w = read_weights(canvas)
    if not col:
        return w
    mw = canvas.matrix_world
    co = np.array([(mw @ v.co)[:2] for v in canvas.data.vertices])
    for o in col.objects:
        c = o.matrix_world.translation
        size = max(o.dimensions)
        d = np.hypot(co[:, 0] - c.x, co[:, 1] - c.y)
        near = d < size * 0.5 + 1.5
        w[near] = np.maximum(w[near], min(1.0, 0.35 + size / 30.0))
    write_weights(canvas, w)
    return w


def undo_last_batch(context, canvas):
    """Remove the last batch's pieces and give its strokes back to the canvas."""
    batch = int(context.scene.get("macerie_batch", 0))
    col = bpy.data.collections.get(OUT_COLLECTION)
    if col:
        for o in list(col.objects):
            if o.get("macerie_batch") == batch:
                bpy.data.objects.remove(o)
    context.scene["macerie_batch"] = max(0, batch - 1)
    write_weights(canvas, read_weights(canvas, LAST_GROUP))


def clear_placed():
    col = bpy.data.collections.get(OUT_COLLECTION)
    if col:
        for o in list(col.objects):
            bpy.data.objects.remove(o)



# ------------------------------------------------------------------ slab templates

SLAB_COLLECTION = "Lastre"
SIDE_MATERIAL = "ctyn-ground-raised-side-01"
FLOOR_MATERIALS = ("city-pal-ground-01", "ctyn-ground-raised-01")
WALL_MATERIALS = ("ctyn-blue-wall-lower-02", "ctyn-green-wall-lower-01", "ctyn-beige-wall-upper-02",
                  "ctyn-black-wall-lower-01", "ctyn-wall-2")
BEAM_MATERIALS = ("ctyn-beams", "ctyn-generic-metal")


def meters_per_uv(material_name):
    """How many meters one UV unit spans where the plaza uses this material (median)."""
    ratios = []
    for o in bpy.data.collections["Jak2 palazzo"].all_objects:
        if o.type != 'MESH' or material_name not in [m.name for m in o.data.materials if m]:
            continue
        me = o.data
        if not me.uv_layers:
            continue
        uv = me.uv_layers[0].data
        slot = [m.name if m else None for m in me.materials].index(material_name)
        for p in me.polygons[:4000]:
            if p.material_index != slot:
                continue
            li = list(p.loop_indices)
            for a, b in zip(li, li[1:] + li[:1]):
                dw = (o.matrix_world @ me.vertices[me.loops[a].vertex_index].co
                      - o.matrix_world @ me.vertices[me.loops[b].vertex_index].co).length
                du = (Vector(uv[a].uv) - Vector(uv[b].uv)).length
                if du > 1e-4 and dw > 0.05:
                    ratios.append(dw / du)
            if len(ratios) > 3000:
                break
        if len(ratios) > 3000:
            break
    return float(np.median(ratios)) if ratios else 4.0


def build_slab(name, outline, thickness, top_mat, side_mat, mpu_top, mpu_side, rng):
    """A thick broken slab: outline (x, y) in meters, bottom at z 0, top textured."""
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    colr = bm.loops.layers.color.new("Color")
    ox, oy = rng.uniform(0, 1), rng.uniform(0, 1)
    top_z = [thickness * rng.uniform(0.9, 1.1) for _ in outline]
    bot = [bm.verts.new((x, y, 0.0)) for x, y in outline]
    top = [bm.verts.new((x, y, z)) for (x, y), z in zip(outline, top_z)]
    ft = bm.faces.new(top)
    ft.material_index = 0
    fb = bm.faces.new(list(reversed(bot)))
    fb.material_index = 1
    sides = []
    n = len(outline)
    for i in range(n):
        j = (i + 1) % n
        f = bm.faces.new((bot[i], bot[j], top[j], top[i]))
        f.material_index = 1
        sides.append(f)
    bm.normal_update()
    for f in bm.faces:
        for lp in f.loops:
            co = lp.vert.co
            if f in sides:
                e = (f.verts[1].co - f.verts[0].co)
                d = Vector((e.x, e.y, 0)).normalized() if e.length > 1e-6 else Vector((1, 0, 0))
                lp[uv].uv = ((co.x * d.x + co.y * d.y) / mpu_side + ox, co.z / mpu_side + oy)
            else:
                lp[uv].uv = (co.x / mpu_top + ox, co.y / mpu_top + oy)
            lp[colr] = (0.54, 0.54, 0.54, 1.0)  # the plaza's mean baked light
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    # the materials read the active color attribute: without it they render black
    me.color_attributes.active_color = me.color_attributes[0]
    me.color_attributes.render_color_index = 0
    me.materials.append(bpy.data.materials[top_mat])
    me.materials.append(bpy.data.materials[side_mat])
    return me


def broken_outline(length, width, rng, sides=None):
    """An irregular, roughly convex broken outline around the origin."""
    k = sides or rng.randint(5, 8)
    angles = sorted(rng.uniform(0, 2 * math.pi) for _ in range(k))
    pts = []
    for a in angles:
        r = rng.uniform(0.7, 1.0)
        pts.append((math.cos(a) * length / 2 * r, math.sin(a) * width / 2 * r))
    return pts


def make_slab_templates(seed=11):
    """Thick broken floor and wall slabs, beams and small fragments in the plaza's materials,
    like the fallen city of Jak 3 (collection "Lastre", hidden; used by the "Crollo" style)."""
    rng = random.Random(seed)
    col = bpy.data.collections.get(SLAB_COLLECTION) or bpy.data.collections.new(SLAB_COLLECTION)
    if col.name not in bpy.context.scene.collection.children:
        bpy.context.scene.collection.children.link(col)
    for o in list(col.objects):
        bpy.data.objects.remove(o)
    mpu = {m: meters_per_uv(m) for m in FLOOR_MATERIALS + WALL_MATERIALS + BEAM_MATERIALS + (SIDE_MATERIAL,)}
    made = []

    def add(name, me):
        ob = bpy.data.objects.new(name, me)
        ob["set_collision"] = 1
        ob["ignore"] = 1
        col.objects.link(ob)
        ob.hide_set(True)
        made.append(ob)

    for i in range(24):  # floor slabs, 3-14 m
        L = rng.uniform(3, 14)
        mat = FLOOR_MATERIALS[0] if rng.random() < 0.75 else FLOOR_MATERIALS[1]
        add(f"lastra_{i:02d}", build_slab(f"lastra_{i:02d}", broken_outline(L, L * rng.uniform(0.45, 0.9), rng),
                                          rng.uniform(0.5, 1.3), mat, SIDE_MATERIAL, mpu[mat], mpu[SIDE_MATERIAL], rng))
    for i in range(14):  # wall chunks, 4-12 m
        L = rng.uniform(4, 12)
        mat = rng.choice(WALL_MATERIALS)
        add(f"muro_{i:02d}", build_slab(f"muro_{i:02d}", broken_outline(L, L * rng.uniform(0.35, 0.7), rng),
                                        rng.uniform(0.4, 0.9), mat, SIDE_MATERIAL, mpu[mat], mpu[SIDE_MATERIAL], rng))
    for i in range(8):  # beams, 6-20 m
        L, t = rng.uniform(6, 20), rng.uniform(0.6, 1.3)
        mat = rng.choice(BEAM_MATERIALS)
        outline = [(-L / 2, -t / 2), (L / 2, -t / 2 * rng.uniform(0.6, 1)), (L / 2, t / 2), (-L / 2, t / 2 * rng.uniform(0.6, 1))]
        add(f"trave_{i:02d}", build_slab(f"trave_{i:02d}", outline, t * rng.uniform(0.7, 1.1), mat, mat, mpu[mat], mpu[mat], rng))
    for i in range(30):  # small fragments, 0.6-3 m
        L = rng.uniform(0.6, 3.0)
        mat = FLOOR_MATERIALS[0] if rng.random() < 0.6 else rng.choice(WALL_MATERIALS)
        add(f"frammento_{i:02d}", build_slab(f"frammento_{i:02d}", broken_outline(L, L * rng.uniform(0.4, 0.9), rng, rng.randint(4, 6)),
                                             rng.uniform(0.12, 0.4), mat, SIDE_MATERIAL, mpu[mat], mpu[SIDE_MATERIAL], rng))
    return made, mpu


# ------------------------------------------------------------------ UI

class MACERIE_OT_canvas(bpy.types.Operator):
    bl_idname = "macerie.canvas"
    bl_label = "Crea tela"
    bl_description = "Griglia sul terreno attorno ai varchi da colorare (tiene il colore già dato)"

    def execute(self, context):
        make_canvas(context)
        return {'FINISHED'}


class MACERIE_OT_paint(bpy.types.Operator):
    bl_idname = "macerie.paint"
    bl_label = "Dipingi a mano"
    bl_description = "Weight Paint sulla tela (gruppo Macerie)"

    def execute(self, context):
        ob = bpy.data.objects.get(CANVAS) or make_canvas(context)
        ob.hide_set(False)
        if context.object and context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        for o in context.selected_objects:
            o.select_set(False)
        ob.select_set(True)
        context.view_layer.objects.active = ob
        ob.vertex_groups.active_index = ob.vertex_groups[GROUP].index
        bpy.ops.object.mode_set(mode='WEIGHT_PAINT')
        # stay on the canvas: a click elsewhere would start painting the level's meshes
        context.scene.tool_settings.lock_object_mode = True
        return {'FINISHED'}


class MACERIE_OT_junctions(bpy.types.Operator):
    bl_idname = "macerie.junctions"
    bl_label = "Dipingi giunzioni"
    bl_description = "Colora da solo i punti dove le giunzioni si vedono dai percorsi"

    def execute(self, context):
        if context.object and context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        ob = bpy.data.objects.get(CANVAS) or make_canvas(context)
        n = paint_junctions(context, ob)
        self.report({'INFO'}, f"{n} punti scoperti")
        return {'FINISHED'}


class MACERIE_OT_place(bpy.types.Operator):
    bl_idname = "macerie.place"
    bl_label = "Piazza macerie"
    bl_description = "Aggiunge pezzi sulle zone colorate, poi cancella il colore (i pezzi già messi restano)"

    def execute(self, context):
        if context.object and context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        ob = bpy.data.objects.get(CANVAS)
        if not ob:
            self.report({'ERROR'}, "Prima crea e colora la tela")
            return {'CANCELLED'}
        made, rejected = place(context, ob, context.scene)
        ob.hide_set(True)
        why = {"walkway": "percorso", "no ground": "senza terreno", "edge": "su uno spigolo", "wall": "nel muro",
               "overlap": "sovrapposti"}
        self.report({'INFO'}, f"{made} pezzi piazzati; scartati: " +
                    (", ".join(f"{n} {why[k]}" for k, n in rejected.most_common()) or "nessuno"))
        return {'FINISHED'}


class MACERIE_OT_clear(bpy.types.Operator):
    bl_idname = "macerie.clear"
    bl_label = "Togli macerie"
    bl_description = "Toglie tutti i pezzi messi dal pennello (non quelli messi a mano)"

    def execute(self, context):
        clear_placed()
        context.scene["macerie_batch"] = 0
        return {'FINISHED'}


class MACERIE_OT_reroll(bpy.types.Operator):
    bl_idname = "macerie.reroll"
    bl_label = "Altra variante"
    bl_description = "Rifà l'ultima piazzata: stesse pennellate, pezzi e posizioni diversi"

    def execute(self, context):
        if context.object and context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        ob = bpy.data.objects.get(CANVAS)
        if not ob:
            return {'CANCELLED'}
        undo_last_batch(context, ob)
        context.scene.macerie_seed += 1
        return bpy.ops.macerie.place()


class MACERIE_OT_slabs(bpy.types.Operator):
    bl_idname = "macerie.slabs"
    bl_label = "Crea lastre"
    bl_description = "Genera lastre spesse, travi e frammenti con i materiali della piazza (stile Crollo)"

    def execute(self, context):
        made, _mpu = make_slab_templates(context.scene.macerie_seed)
        self.report({'INFO'}, f"{len(made)} lastre")
        return {'FINISHED'}


class MACERIE_PT_panel(bpy.types.Panel):
    bl_label = "Macerie"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Macerie"

    def draw(self, context):
        s = context.scene
        col = self.layout.column(align=True)
        col.operator("macerie.canvas", icon='MESH_GRID')
        col.operator("macerie.paint", icon='BRUSH_DATA')
        col.operator("macerie.junctions", icon='VIEWZOOM')
        col.operator("macerie.slabs", icon='MOD_SOLIDIFY')
        col.separator()
        col.prop(s, "macerie_stile")
        col.prop(s, "macerie_densita")
        col.prop(s, "macerie_min")
        col.prop(s, "macerie_max")
        col.prop(s, "macerie_inclinazione")
        col.prop(s, "macerie_affonda")
        col.prop(s, "macerie_seed")
        col.separator()
        col.operator("macerie.place", icon='PARTICLES')
        col.operator("macerie.reroll", icon='FILE_REFRESH')
        col.operator("macerie.clear", icon='TRASH')


CLASSES = (MACERIE_OT_slabs, MACERIE_OT_canvas, MACERIE_OT_paint, MACERIE_OT_junctions, MACERIE_OT_place, MACERIE_OT_clear,
           MACERIE_OT_reroll, MACERIE_PT_panel)


def register():
    S = bpy.types.Scene
    S.macerie_densita = bpy.props.FloatProperty(name="Densità (per 100 m²)", default=4.0, min=0.05, max=20)
    S.macerie_min = bpy.props.FloatProperty(name="Grandezza min (m)", default=3.0, min=0.5, max=80)
    S.macerie_max = bpy.props.FloatProperty(name="Grandezza max (m)", default=25.0, min=1, max=120)
    S.macerie_inclinazione = bpy.props.FloatProperty(name="Inclinazione max (°)", default=22, min=0, max=80)
    S.macerie_affonda = bpy.props.FloatProperty(name="Affondamento", default=0.08, min=0, max=0.6,
                                                description="Quanto il pezzo entra nel terreno, in parte della sua altezza")
    S.macerie_seed = bpy.props.IntProperty(name="Variante", default=1)
    S.macerie_stile = bpy.props.EnumProperty(name="Stile", default='CROLLO', items=(
        ('CROLLO', "Crollo", "Come la città crollata di Jak 3: lastre e travi sdraiate o appoggiate, pezzetti attorno"),
        ('SCHEGGE', "Schegge", "Pezzi inclinati a caso")))
    for c in CLASSES:
        if hasattr(bpy.types, c.__name__):
            bpy.utils.unregister_class(getattr(bpy.types, c.__name__))
        bpy.utils.register_class(c)


if __name__ == "__main__":
    register()
