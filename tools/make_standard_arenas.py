"""The standard arenas: arenas that come with the recomp for every player, made from the player's own disc (nothing of
the game is in the repo, only this code that reshapes it).

The first: The Face Club with a six-sided ring. Its folder has the form of a mod's arena (model.arena in the game's
arena format, arena.json with "ring"), so the game draws it as any mod arena and the mod tools read it. Everything of
The Face Club stays its own (the building, the crowd, the lights, the textures); the square ring's parts are taken out
and six-sided ones put in:
- the mat's top (scenery part 0, the batches flat at 10.5 inside +-40.3): a hexagon, its flat sides where the square's
  were (40.25), its corners at 46.48, the same texture laid the same way, the baked light (vertex colours) the square's
  in the middle and its edge's light along each of the six sides;
- the mat's side (the batch from the floor to 10.5 at +-40.26): six sides, the fence twice along each;
- the shadow floor (part 99): the new mat's and floor's own triangles (the game draws it with depth "greater or
  equal" over them, so it must be the same triangles), the square's kept past the floor's outer edge;
- the floor round the ring (part 0's batch flat on the ground out to the barricade): from the hexagon's foot out, with
  one picture of its own laid flat (the same tiles, the same painted light, the six-sided ring's shadows);
- the mat's and the floor's pictures (textures\\@<address>.png): the mat's with its painted shading redrawn for six
  sides and six corners, the floor's as above; the game draws them in place of its own at those addresses;
- the corner pieces (81-92: posts, pads, turnbuckles): six of each, at part numbers 1000 + corner (posts), 1020 +
  corner (pads), 1040 + corner (turnbuckles). A post is moved and turned to each corner, the same distance outside the
  ring; a pad and a turnbuckle are reshaped to the corner, each point kept at its distance from the corner's two ropes.
The ring's corners as the fight's ring code puts them: corner k at atan2(x, z) = 90 - 60k degrees, side k from corner k
to corner k + 1.

usage: python tools/make_standard_arenas.py [--game <the game folder>] [--out <folder>] [--name <a test mod's name>]
       (the game folder: game\\ beside this tools\\ folder; by default the arena is written into its
       standard\\arenas\\THE FACE CLUB SIX SIDES\\, where the game finds it; the setup runs it, then takes its Choose
       Stage pictures with the game: picture_setting(), picture_match())
"""
import argparse
import json
import math
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# ---- the game's files

TOC_AT = 0x157F40        # the executable's chunk table (address 0x256F40): {u32 first sector, u32 sectors} a chunk
SECTOR = 2048
SCENE_CHUNK, TEXTURE_CHUNK = 140, 141   # + 2 x venue (High Poly)
CROWD_CHUNKS = {7: (272, 274)}          # The Face Club's 3-D crowd: its people, their clips
SERIAL = b'SLUS-20639'


class Game:
    def __init__(self, folder):
        exe = os.path.join(folder, 'SLUS_206.39')
        self.data = os.path.join(folder, 'DATA', 'DATA.BIN')
        if not os.path.isfile(exe) or not os.path.isfile(self.data):
            raise SystemExit(f'the game\'s files are not in {folder} (prepare the disc first)')
        self.exe = open(exe, 'rb').read()

    def chunk(self, n):
        first, count = struct.unpack_from('<2I', self.exe, TOC_AT + 8 * n)
        with open(self.data, 'rb') as f:
            f.seek(first * SECTOR)
            return f.read(count * SECTOR)


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def i32(d, o):
    return struct.unpack_from('<i', d, o)[0]


# ---- the scene: {u32 parts, 16 bytes, then {i32 id, u32 offset from the next word} a part}, the parts

class Part:
    def __init__(self, pid, data):
        self.id, self.data = pid, data


def read_scene(scene):
    n = u32(scene, 0)
    entries = []
    for k in range(n):
        at = 0x14 + 8 * k
        entries.append((i32(scene, at), (at + 4 + u32(scene, at + 4)) & 0xFFFFFFFF))
    starts = sorted({o for _, o in entries}) + [len(scene)]
    following = {o: starts[i + 1] for i, o in enumerate(starts[:-1])}
    return scene[4:0x14], [Part(pid, scene[o:following[o]]) for pid, o in entries]


def write_scene(head, parts):
    out = bytearray(struct.pack('<I', len(parts)) + head)
    out += bytes(8 * len(parts))
    for k, p in enumerate(parts):
        out += bytes(-len(out) % 16)
        struct.pack_into('<iI', out, 0x14 + 8 * k, p.id, (len(out) - (0x18 + 8 * k)) & 0xFFFFFFFF)
        out += p.data
    out += bytes(-len(out) % 16)
    return bytes(out)


# ---- a mesh part: {u32 quadwords, 0x7C zero bytes}, then a VIF1 stream: its head, the material table (an UNPACK to VU
# 0x39: 7 rows of set-up, then {TEX0, register} a slot), then the batches, each an UNPACK with the FLG bit, FLUSH, MSCALF
# (0xC textured, 0x10 the shadow kernel), NOP

TEXTURED, SHADOW = 0x0C, 0x10
TABLE_AT, TABLE_ROWS = 0x39, 7
VIF_NOP, VIF_FLUSH = 0x00000000, 0x11000000


def vif_walk(d, o, end):
    """The VIF1 commands from o to end: (offset of the command, command, immediate, count, offset of its data)."""
    out = []
    while o + 4 <= end:
        imm, num, cmd = d[o] | (d[o + 1] << 8), d[o + 2], d[o + 3] & 0x7F
        at = o
        o += 4
        if cmd >= 0x60:
            vn, vl, count = (cmd >> 2) & 3, cmd & 3, num or 256
            out.append((at, cmd, imm, count, o))
            o += (count * (vn + 1) * (32 >> vl) // 8 + 3) & ~3
            continue
        if cmd == 0x20:
            o += 4
        elif cmd in (0x30, 0x31):
            o += 16
        elif cmd == 0x4A:
            o += (num or 256) * 8
        elif cmd in (0x50, 0x51):
            o += (imm or 65536) * 16
        elif cmd not in (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x13, 0x14, 0x15, 0x17):
            raise SystemExit(f'a VIF command 0x{cmd:02X} no arena uses')
        out.append((at, cmd, imm, num, o))
    return out


class Batch:
    """prog, slot (a textured batch's material slot; a shadow batch's own word), vertices (textured: (s, t, r, g, b, a,
    x, y, z), r g b a 0 to 128; shadow: (x, y, z)), strips (lists of vertex numbers), loose triangles."""

    def __init__(self, prog, slot, vertices, strips, loose):
        self.prog, self.slot, self.vertices, self.strips, self.loose = prog, slot, vertices, strips, loose

    def triangles(self):
        out = []
        for s in self.strips:
            out += [(s[i], s[i + 1], s[i + 2]) for i in range(len(s) - 2)]
        return out + [tuple(t) for t in self.loose]


class Mesh:
    def __init__(self, head, prefix, slots, between, batches):
        self.head, self.prefix, self.slots, self.between, self.batches = head, prefix, slots, between, batches


def read_batch(d, at, rows, prog, top):
    strips_n, loose_n, verts_n, word = struct.unpack_from('<4I', d, at + 32)
    size = 3 if prog == TEXTURED else 2
    vo = at + 48
    if prog == TEXTURED:
        vertices = []
        for k in range(verts_n):
            s, t, _, _, r, g, b, a, x, y, z, _ = struct.unpack_from('<12f', d, vo + 48 * k)
            vertices.append((s, t, r, g, b, a, x, y, z))
    else:
        vertices = [struct.unpack_from('<3f', d, vo + 32 * k + 16) for k in range(verts_n)]
    w = vo + 16 * size * verts_n

    def number(address):
        k = address - top - 3
        if k < 0 or k % size or k // size >= verts_n:
            raise SystemExit('a batch names an address between its vertices')
        return k // size
    strips, loose = [], []
    for _ in range(strips_n):
        c = u32(d, w)
        strips.append([number(u32(d, w + 4 + 4 * i)) for i in range(c)])
        w += 4 + 4 * c
    for _ in range(loose_n):
        loose.append(tuple(number(u32(d, w + 4 * i)) for i in range(3)))
        w += 12
    if w > at + 16 * rows:
        raise SystemExit('a batch runs past its rows')
    return Batch(prog, word - 0x40 if prog == TEXTURED else word, vertices, strips, loose)


def read_mesh(data):
    q = u32(data, 0)
    end = 0x80 + 16 * q
    table_at = table = first = pending = None
    batches = []
    for at, cmd, imm, count, dat in vif_walk(data, 0x80, end):
        if cmd >= 0x60:
            if imm & 0x8000:
                if first is None:
                    first = at
                pending = (dat, count)
            elif imm & 0x3FF == TABLE_AT and table_at is None:
                table_at, table = at, (dat, count)
        elif cmd in (0x14, 0x15, 0x17) and pending is not None:
            top = 0x200 if len(batches) % 2 == 0 else 0x100
            batches.append(read_batch(data, pending[0], pending[1], imm, top))
            pending = None
    dat, count = table
    prefix = data[dat:dat + 16 * TABLE_ROWS]
    slots = [struct.unpack_from('<2Q', data, dat + 16 * k) for k in range(TABLE_ROWS, count)]
    between = data[dat + 16 * count:first if first is not None else end]
    return Mesh(data[0x80:table_at], prefix, slots, between, batches)


def batch_bytes(b, top):
    size = 3 if b.prog == TEXTURED else 2
    xs, ys, zs = ([v[-3 + i] for v in b.vertices] for i in range(3))
    box = (min(xs), min(ys), min(zs), 1.0, max(xs), max(ys), max(zs), 1.0)
    word = 0x40 + b.slot if b.prog == TEXTURED else b.slot
    out = bytearray(struct.pack('<8f4I', *box, len(b.strips), len(b.loose), len(b.vertices), word))
    for v in b.vertices:
        if b.prog == TEXTURED:
            s, t, r, g, bl, a, x, y, z = v
            out += struct.pack('<12f', s, t, 1.0, 1.0, r, g, bl, a, x, y, z, 1.0)
        else:
            out += struct.pack('<8f', 0.0, 0.0, 1.0, 1.0, v[0], v[1], v[2], 1.0)
    for s in b.strips:
        out += struct.pack('<I', len(s)) + struct.pack(f'<{len(s)}I', *(top + 3 + size * i for i in s))
    for t in b.loose:
        out += struct.pack('<3I', *(top + 3 + size * i for i in t))
    out += bytes(-len(out) % 16)
    # what the kernel writes for it: a GIF tag and 3 quadwords a strip vertex, 9 a loose triangle, the end
    written = sum(1 + 3 * len(s) for s in b.strips) + 1 + 9 * len(b.loose)
    if len(out) // 16 > 256 or written > 254:
        raise SystemExit(f'a batch came out {len(out) // 16} / {written} quadwords (the game takes 256 / 254)')
    return bytes(out)


def write_mesh(m):
    s = bytearray(m.head)
    s += struct.pack('<I', (0x6C << 24) | (((TABLE_ROWS + len(m.slots)) & 0xFF) << 16) | TABLE_AT)
    s += m.prefix
    for tex0, reg in m.slots:
        s += struct.pack('<QQ', tex0, reg)
    s += m.between
    for k, b in enumerate(m.batches):
        data = batch_bytes(b, 0x200 if k % 2 == 0 else 0x100)
        if k:
            s += struct.pack('<I', VIF_NOP)
        while len(s) % 16 != 12:
            s += struct.pack('<I', VIF_NOP)
        s += struct.pack('<I', (0x6C << 24) | (((len(data) // 16) & 0xFF) << 16) | 0x8000)
        s += data
        s += struct.pack('<II', VIF_FLUSH, (0x15 << 24) | b.prog)
    if m.batches:
        s += struct.pack('<I', VIF_FLUSH)
    while len(s) % 16:
        s += struct.pack('<I', VIF_NOP)
    return struct.pack('<I', len(s) // 16) + bytes(0x7C) + bytes(s)


# ---- a corner piece (the High Poly posts, pads, turnbuckles): {u32 vertices, u32 triangles, u64 TEX0}, 64-byte
# vertices [s t 1 0] [r g b a] [the game's scratch] [x y z 0], u16 triangles

def read_post(data):
    nv, nt, tex0 = struct.unpack_from('<IIQ', data, 0)
    vertices = [bytearray(data[16 + 64 * k:16 + 64 * (k + 1)]) for k in range(nv)]
    at = 16 + 64 * nv
    triangles = [struct.unpack_from('<3H', data, at + 6 * k) for k in range(nt)]
    return tex0, vertices, triangles


def write_post(tex0, vertices, triangles):
    out = bytearray(struct.pack('<IIQ', len(vertices), len(triangles), tex0))
    for v in vertices:
        out += v
    for t in triangles:
        out += struct.pack('<3H', *t)
    return bytes(out)


def post_xyz(v):
    return struct.unpack_from('<3f', v, 48)


# ---- the ring's numbers (the game's square and the six-sided ring as the fight's ring code builds it)

SIDES = 6
FIRST = 90.0                 # corner 0's angle, atan2(x, z) in degrees
MAT = 40.25                  # the mat's flat sides: the square's half width
MAT_Y = 10.5
ROPES = 35.0                 # the ropes' distance (the corner pieces are fitted to them)
HALF_SIDE = MAT * math.tan(math.pi / SIDES)
POST, PAD, TURNBUCKLE = 1000, 1020, 1040


def angle_vec(degrees):
    a = math.radians(degrees)
    return math.sin(a), math.cos(a)


def corner(k, d):
    """Corner k of the ring whose flat sides are d from the middle."""
    r = d / math.cos(math.pi / SIDES)
    x, z = angle_vec(FIRST - 360.0 / SIDES * k)
    return x * r, z * r


def side_normal(k):
    return angle_vec(FIRST - 360.0 / SIDES * (k + 0.5))


def side_tangent(k):
    """Along side k from corner k to corner k + 1."""
    nx, nz = side_normal(k)
    return -nz, nx


def turn(x, z, degrees):
    """(x, z) turned by `degrees` in the angle's own sense (atan2(x, z) grows by it)."""
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    return x * c + z * s, z * c - x * s


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1]


# ---- the square's own light and texture, looked up

def barycentric(p, a, b, c):
    d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if abs(d) < 1e-12:
        return None
    w0 = ((b[1] - c[1]) * (p[0] - c[0]) + (c[0] - b[0]) * (p[1] - c[1])) / d
    w1 = ((c[1] - a[1]) * (p[0] - c[0]) + (a[0] - c[0]) * (p[1] - c[1])) / d
    return w0, w1, 1.0 - w0 - w1


class SquareMat:
    """The square mat's triangles (x, z) with their vertex colours, and how its texture is laid (s, t from x, z)."""

    def __init__(self, batches):
        self.tris = []
        points = []
        for b in batches:
            for t in b.triangles():
                vs = [b.vertices[i] for i in t]
                self.tris.append([((v[6], v[8]), v[2:6]) for v in vs])
            points += b.vertices
        self.s = fit_plane([(v[6], v[8], v[0]) for v in points])
        self.t = fit_plane([(v[6], v[8], v[1]) for v in points])

    def light(self, x, z):
        x = max(-MAT, min(MAT, x))
        z = max(-MAT, min(MAT, z))
        best, best_out = None, 1e30
        for tri in self.tris:
            w = barycentric((x, z), tri[0][0], tri[1][0], tri[2][0])
            if w is None:
                continue
            out = -min(w)
            if out < best_out:
                best_out, best = out, (tri, w)
        tri, w = best
        w = [max(0.0, q) for q in w]
        total = sum(w) or 1.0
        return tuple(sum(w[i] * tri[i][1][c] for i in range(3)) / total for c in range(4))

    def uv(self, x, z):
        return (self.s[0] * x + self.s[1] * z + self.s[2], self.t[0] * x + self.t[1] * z + self.t[2])


def fit_plane(samples):
    """value = a x + b z + c, least squares."""
    sxx = sxz = szz = sx = sz = n = sxv = szv = sv = 0.0
    for x, z, v in samples:
        sxx += x * x
        sxz += x * z
        szz += z * z
        sx += x
        sz += z
        n += 1
        sxv += x * v
        szv += z * v
        sv += v
    m = [[sxx, sxz, sx], [sxz, szz, sz], [sx, sz, n]]
    r = [sxv, szv, sv]

    def det3(a):
        return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    d = det3(m)
    out = []
    for col in range(3):
        mm = [row[:] for row in m]
        for i in range(3):
            mm[i][col] = r[i]
        out.append(det3(mm) / d)
    worst = max(abs(out[0] * x + out[1] * z + out[2] - v) for x, z, v in samples)
    if worst > 0.01:
        raise SystemExit(f'the mat\'s texture is not laid flat over it (off by {worst:.3f})')
    return out


# The square's four sides as (outward normal, along) for the edge's light
SQUARE_SIDES = [((0.0, 1.0), (-1.0, 0.0)), ((1.0, 0.0), (0.0, 1.0)), ((0.0, -1.0), (1.0, 0.0)), ((-1.0, 0.0), (0.0, -1.0))]
EDGE_ONLY, MIDDLE_ONLY = 8.0, 16.0   # how far in from the edge the edge's light gives way to the square's own


def hex_light(square, x, z):
    """The six-sided mat's light at (x, z): within EDGE_ONLY of an edge, the square's edge light at the same distance
    from its edge and the same share of the way to a corner (all four sides and both ways averaged); past MIDDLE_ONLY,
    the square's own light at the same place; between, the one blended into the other."""
    k = max(range(SIDES), key=lambda i: dot(side_normal(i), (x, z)))
    depth = MAT - dot(side_normal(k), (x, z))
    along = dot(side_tangent(k), (x, z)) * (MAT / HALF_SIDE)
    edge = [0.0] * 4
    for n, t in SQUARE_SIDES:
        for sign in (1.0, -1.0):
            px = n[0] * (MAT - depth) + t[0] * along * sign
            pz = n[1] * (MAT - depth) + t[1] * along * sign
            c = square.light(px, pz)
            for i in range(4):
                edge[i] += c[i] / 8.0
    if depth <= EDGE_ONLY:
        return tuple(edge)
    own = square.light(x, z)
    if depth >= MIDDLE_ONLY:
        return own
    w = (depth - EDGE_ONLY) / (MIDDLE_ONLY - EDGE_ONLY)
    return tuple(edge[i] * (1 - w) + own[i] * w for i in range(4))


# ---- the six-sided ring's parts

def mat_batches(square, slot, uv, steps=6):
    """The mat's top: a batch a side, the triangle from the middle to the side cut into steps x steps; uv(x, z) its
    texture coordinates."""
    out = []
    for k in range(SIDES):
        a, b = corner(k, MAT), corner(k + 1, MAT)
        vertices, number = [], {}
        for i in range(steps + 1):
            for j in range(i + 1):
                x = a[0] * i / steps + (b[0] - a[0]) * j / steps
                z = a[1] * i / steps + (b[1] - a[1]) * j / steps
                s, t = uv(x, z)
                r, g, bl, al = hex_light(square, x, z)
                number[(i, j)] = len(vertices)
                vertices.append((s, t, r, g, bl, al, x, MAT_Y, z))
        strips = []
        for i in range(steps):
            strip = []
            for j in range(i + 1):
                strip += [number[(i + 1, j)], number[(i, j)]]
            strip.append(number[(i + 1, i + 1)])
            strips.append(strip)
        out.append(Batch(TEXTURED, slot, vertices, strips, []))
    return out


def skirt_batch(square_skirt):
    """The mat's side: six sides from the floor to the mat, each in two halves with the fence once across a half, the
    light the square's (its top in a side's middle, at a corner; its bottom)."""
    vs = square_skirt.vertices
    tops = [v for v in vs if v[7] > MAT_Y - 0.01]
    bottoms = [v for v in vs if v[7] < 0.01]
    top_t, bottom_t = tops[0][1], bottoms[0][1]
    s_end = max(v[0] for v in vs)

    def average(group, at_corner):
        chosen = [v for v in group if (min(abs(v[6]), abs(v[8])) > MAT - 0.1) == at_corner]
        return tuple(sum(v[2 + i] for v in chosen) / len(chosen) for i in range(4))
    light = {(True, True): average(tops, True), (True, False): average(tops, False),
             (False, True): average(bottoms, True), (False, False): average(bottoms, False)}
    top_out = max(max(abs(v[6]), abs(v[8])) for v in tops)        # 40.25
    bottom_out = max(max(abs(v[6]), abs(v[8])) for v in bottoms)  # 40.26
    vertices, strips = [], []
    for k in range(SIDES):
        for half in range(2):
            strip = []
            for along, s in ((half * 0.5, 0.0), (half * 0.5 + 0.5, s_end)):
                for top in (True, False):
                    out = top_out if top else bottom_out
                    a, b = corner(k, out), corner(k + 1, out)
                    x, z = a[0] + (b[0] - a[0]) * along, a[1] + (b[1] - a[1]) * along
                    strip.append(len(vertices))
                    vertices.append((s, top_t if top else bottom_t) + light[(top, along != 0.5)] +
                                    (x, MAT_Y if top else 0.0, z))
            strips.append(strip)
    return Batch(TEXTURED, square_skirt.slot, vertices, strips, [])


def shadow_floor(old, surfaces, floor_out):
    """Part 99, the floor the wrestlers' shadows fall on. The game draws it over what it lies on with depth "greater or
    equal" (TEST_2 0x5001B), so it shows only where it is made of the very same triangles as the surface below (the
    square's mat and its part 99 share theirs): the new mat's and floor's batches copied (their positions, strips and
    loose triangles); of the square's own, only what lies past the floor's outer edge (the barricade's step) is kept."""
    keep = []
    for b in old.batches:
        def inside(i):
            x, y, z = b.vertices[i]
            return abs(y) < 0.01 and max(abs(x), abs(z)) <= floor_out + 0.01
        tris = [t for t in b.triangles() if not all(abs(b.vertices[i][1] - MAT_Y) < 0.01 for i in t)
                and not all(inside(i) for i in t)]
        if not tris:
            continue
        if len(tris) == len(b.triangles()):
            keep.append(b)
            continue
        for at in range(0, len(tris), 28):   # 28 loose triangles a batch: 9 quadwords each for the kernel
            part = tris[at:at + 28]
            used = sorted({i for t in part for i in t})
            renumber = {i: n for n, i in enumerate(used)}
            keep.append(Batch(b.prog, b.slot, [b.vertices[i] for i in used], [],
                              [tuple(renumber[i] for i in t) for t in part]))
    word = old.batches[0].slot
    copies = [Batch(SHADOW, word, [(v[6], v[7], v[8]) for v in s.vertices], [list(x) for x in s.strips],
                    [tuple(x) for x in s.loose]) for s in surfaces]
    return keep + copies


def square_corner_angle(vertices):
    xs = [post_xyz(v)[0] for v in vertices]
    zs = [post_xyz(v)[2] for v in vertices]
    mx, mz = (min(xs) + max(xs)) / 2, (min(zs) + max(zs)) / 2
    return math.degrees(math.atan2(mx, mz)), (mx, mz)


def to_corner(x, z, from_angle, k):
    """A point near the square's corner at `from_angle` put at the ring's corner k, keeping its distances from the
    corner's two ropes (the 90 degree corner opened to the ring's)."""
    na, nb = angle_vec(from_angle + 45.0), angle_vec(from_angle - 45.0)
    da, db = ROPES - dot(na, (x, z)), ROPES - dot(nb, (x, z))
    theta = FIRST - 360.0 / SIDES * k
    ha, hb = angle_vec(theta + 180.0 / SIDES), angle_vec(theta - 180.0 / SIDES)
    ra, rb = ROPES - da, ROPES - db
    det = ha[0] * hb[1] - ha[1] * hb[0]
    return (ra * hb[1] - rb * ha[1]) / det, (ha[0] * rb - hb[0] * ra) / det


def corner_pieces(post, pad, turnbuckle):
    """Six of each: the post moved and turned (it stands square to the corner's middle line), the pad and the
    turnbuckle reshaped to the corner (to_corner)."""
    parts = []
    tex0, vertices, triangles = post
    from_angle, (mx, mz) = square_corner_angle(vertices)
    flat = abs(mx)
    for k in range(SIDES):
        theta = FIRST - 360.0 / SIDES * k
        cx, cz = corner(k, flat)
        moved = []
        for v in vertices:
            x, y, z = post_xyz(v)
            dx, dz = turn(x - mx, z - mz, theta - from_angle)
            w = bytearray(v)
            struct.pack_into('<3f', w, 48, cx + dx, y, cz + dz)
            moved.append(w)
        parts.append(Part(POST + k, write_post(tex0, moved, triangles)))
    for base, (tex0, vertices, triangles) in ((PAD, pad), (TURNBUCKLE, turnbuckle)):
        from_angle, _ = square_corner_angle(vertices)
        for k in range(SIDES):
            moved = []
            for v in vertices:
                x, y, z = post_xyz(v)
                nx, nz = to_corner(x, z, from_angle, k)
                w = bytearray(v)
                struct.pack_into('<3f', w, 48, nx, y, nz)
                moved.append(w)
            parts.append(Part(base + k, write_post(tex0, moved, triangles)))
    return parts


# ---- pictures: the arena's own textures read from its part 1, the new ones written as PNG (textures\@<address>.png,
# the address in hexadecimal: the game's texture there is drawn with the PNG instead)

def texture_uploads(textures):
    """The part's uploads by GS address: (offset of the pixels, width, height, format). Each is a GIF packet {a tag of
    four A+D registers: BITBLTBUF, TRXPOS, TRXREG, TRXDIR}, then the IMAGE tag and the pixels."""
    tag = bytes((1, 0, 0, 0, 0, 0, 0, 0x40))
    out, o = {}, 0
    while True:
        o = textures.find(tag, o)
        if o < 0 or o + 0x60 > len(textures):
            return out
        if (textures[o + 0x18], textures[o + 0x28], textures[o + 0x38], textures[o + 0x48]) == (0x50, 0x51, 0x52, 0x53):
            bitblt = struct.unpack_from('<Q', textures, o + 0x10)[0]
            size = struct.unpack_from('<Q', textures, o + 0x30)[0]
            out.setdefault((bitblt >> 32) & 0x3FFF, (o + 0x60, size & 0xFFF, (size >> 32) & 0xFFF, (bitblt >> 56) & 0x3F))
        o += 16


class Picture:
    """RGBA pixels, 0 to 255 a channel (alpha as the GS's: 128 = solid)."""

    def __init__(self, width, height, data):
        self.width, self.height, self.data = width, height, data

    def sample(self, u, v):
        """(r, g, b, a) at texel (u, v), bilinear, wrapping (texel centres at whole numbers)."""
        w, h, d = self.width, self.height, self.data
        fu, fv = math.floor(u), math.floor(v)
        au, av = u - fu, v - fv
        x0, y0 = int(fu) % w, int(fv) % h
        x1, y1 = (x0 + 1) % w, (y0 + 1) % h
        i00, i10, i01, i11 = 4 * (y0 * w + x0), 4 * (y0 * w + x1), 4 * (y1 * w + x0), 4 * (y1 * w + x1)
        w00, w10, w01, w11 = (1 - au) * (1 - av), au * (1 - av), (1 - au) * av, au * av
        return [d[i00 + c] * w00 + d[i10 + c] * w10 + d[i01 + c] * w01 + d[i11 + c] * w11 for c in range(4)]

    def at(self, x, y):
        i = 4 * ((y % self.height) * self.width + (x % self.width))
        return self.data[i:i + 4]


def eight_bit_picture(textures, tex0):
    """An 8-bit texture (TEX0) whose pixels were sent as 32-bit words (as the arenas send theirs) and whose palette is
    16 x 16 32-bit colours, as RGBA. A 32-bit block and an 8-bit block keep their bytes in different orders; this is the
    mapping between the two for a picture sent from its first block, and the palette's own order (CSM1)."""
    up = texture_uploads(textures)
    tbp, cbp = tex0 & 0x3FFF, (tex0 >> 37) & 0x3FFF
    width, height = 1 << ((tex0 >> 26) & 0xF), 1 << ((tex0 >> 30) & 0xF)
    if (tex0 >> 20) & 0x3F != 0x13 or tbp not in up or cbp not in up:
        raise SystemExit(f'the texture at {tbp:#06x} is not an 8-bit one this knows')
    at, w, h, psm = up[tbp]
    pal_at, pw, ph, ppsm = up[cbp]
    if psm != 0 or ppsm != 0 or (2 * w, 2 * h) != (width, height) or pw * ph != 256:
        raise SystemExit(f'the texture at {tbp:#06x} is not sent as this knows')
    palette = [None] * 256
    for k in range(256):
        palette[(k & 0xE7) | ((k & 8) << 1) | ((k & 0x10) >> 1)] = textures[pal_at + 4 * k:pal_at + 4 * k + 4]
    out = bytearray(4 * width * height)
    o = 0
    for y in range(height):
        iy = y & 15
        sub = (iy >> 1) & 1
        row = (((y >> 4) << 3) | ((iy >> 2) << 1) | (iy & 1)) * w
        swap = ((sub ^ (iy >> 2)) & 1) << 2
        for x in range(width):
            col = ((x >> 4) << 3) | ((x & 7) ^ swap)
            out[o:o + 4] = palette[textures[at + (row + col) * 4 + (sub | (((x >> 3) & 1) << 1))]]
            o += 4
    return Picture(width, height, out)


def png(width, height, rows):
    """A PNG (8-bit RGBA) from its rows of bytes."""
    import zlib

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def gs_alpha(a):
    return min(255, int(a * 255 / 128 + 0.5))


def smooth(edge0, edge1, x):
    if edge0 == edge1:
        return 1.0 if x >= edge1 else 0.0
    t = min(1.0, max(0.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3 - 2 * t)


def bright8(values):
    """The second brightest of eight: the cloth's own shade where the logo's ink covers some of the eight places."""
    return sorted(values)[-2]


# The mat's picture. The square's has the ropes' and corners' shading painted in: darker in a band round its edge and in
# a mark along each corner's middle line. Its shading is measured as a function of the distance in from the edge (e) and
# the share of the way along that edge to a corner (w, 1 on the middle line): the second brightest of the eight places
# with the same (e, w), so the logo's ink on some of them doesn't count, over the cloth further in. The new picture is the square's own at
# the same place (the logo where it was), its square shading divided out and the same shading for the hexagon's (e, w)
# put in. Past MAT_SHADED from every edge nothing changes. The hexagon's points reach past the square: there the
# square's picture is taken mirrored back in across its edge.
MAT_SHADED = 22.0
MAT_FIT = 0.86            # the texture laid this much smaller over the mat, so the hexagon's points fit inside it
MAT_PICTURE = 512


class MatPicture:
    def __init__(self, picture, square):
        self.pic, self.square = picture, square
        self.e_steps, self.w_steps = int(MAT_SHADED * 5), 20   # every 0.2 in: the painted lines are a texel wide
        table = []
        for i in range(self.e_steps + 1):
            e = MAT_SHADED * i / self.e_steps
            row = []
            for j in range(self.w_steps + 1):
                w = j / self.w_steps
                samples = []
                for n, t in SQUARE_SIDES:
                    for sign in (1.0, -1.0):
                        x = n[0] * (MAT - e) + t[0] * w * (MAT - e) * sign
                        z = n[1] * (MAT - e) + t[1] * w * (MAT - e) * sign
                        samples.append(self.old(x, z))
                row.append([bright8([s[c] for s in samples]) for c in range(3)])
            table.append(row)
        # shading relative to the cloth MAT_SHADED in at the same w (so it is 1 there and nothing changes past it)
        self.table = [[[table[i][j][c] / max(1.0, table[-1][j][c]) for c in range(3)] for j in range(self.w_steps + 1)]
                      for i in range(self.e_steps + 1)]

    def old(self, x, z):
        s, t = self.square.uv(x, z)
        return self.pic.sample(s * self.pic.width - 0.5, t * self.pic.height - 0.5)

    def shading(self, e, w):
        if e >= MAT_SHADED:
            return (1.0, 1.0, 1.0)
        fi = max(0.0, e) / MAT_SHADED * self.e_steps
        fj = min(1.0, max(0.0, w)) * self.w_steps
        i, j = min(int(fi), self.e_steps - 1), min(int(fj), self.w_steps - 1)
        a, b = fi - i, fj - j
        t = self.table
        return [t[i][j][c] * (1 - a) * (1 - b) + t[i + 1][j][c] * a * (1 - b) + t[i][j + 1][c] * (1 - a) * b +
                t[i + 1][j + 1][c] * a * b for c in range(3)]

    def colour(self, x, z):
        k = max(range(SIDES), key=lambda i: dot(side_normal(i), (x, z)))
        e_hex = MAT - dot(side_normal(k), (x, z))
        reach = HALF_SIDE * max(1e-6, MAT - max(0.0, e_hex)) / MAT
        w_hex = abs(dot(side_tangent(k), (x, z))) / reach
        sx, sz = x, z
        if abs(sx) > MAT:
            sx = math.copysign(2 * MAT - abs(sx), sx)
        if abs(sz) > MAT:
            sz = math.copysign(2 * MAT - abs(sz), sz)
        big, small = max(abs(sx), abs(sz)), min(abs(sx), abs(sz))
        e_square = MAT - big
        w_square = small / max(1e-6, big)
        old = self.old(sx, sz)
        new = self.shading(e_hex, w_hex)
        was = self.shading(e_square, w_square)
        return [min(255.0, old[c] * new[c] / max(0.15, was[c])) for c in range(3)] + [old[3]]

    def make(self, size=MAT_PICTURE):
        """The new picture over the mat's new texture coordinates (new_uv), as PNG rows."""
        s = self.square
        c_s, c_t = s.uv(0.0, 0.0)
        det = s.s[0] * s.t[1] - s.s[1] * s.t[0]
        rows = []
        for r in range(size):
            tn = (r + 0.5) / size - 1.0
            # the representative of t (mod 1) nearest the middle's
            tn += round(c_t - tn)
            row = bytearray()
            for i in range(size):
                sn = (i + 0.5) / size
                so, to = c_s + (sn - c_s) / MAT_FIT, c_t + (tn - c_t) / MAT_FIT
                ds, dt = so - s.s[2], to - s.t[2]
                x = (ds * s.t[1] - dt * s.s[1]) / det
                z = (s.s[0] * dt - s.t[0] * ds) / det
                col = self.colour(x, z)
                row += bytes((int(col[0] + 0.5), int(col[1] + 0.5), int(col[2] + 0.5), gs_alpha(col[3])))
            rows.append(row)
        return rows

    def new_uv(self, x, z):
        c_s, c_t = self.square.uv(0.0, 0.0)
        s, t = self.square.uv(x, z)
        return c_s + (s - c_s) * MAT_FIT, c_t + (t - c_t) * MAT_FIT


# The floor round the ring. The square's (scenery batch 127): four trapezoids from 44.28 out to the barricade at +-76,
# each with the 256 x 128 tile picture laid once (its rows outward from the ring, its columns along the side, its edges
# on the diagonals), and a band from the mat's edge to 44.28 with the picture's first 14 rows. Painted into the picture:
# the light (brighter near the ring and in a side's middle), the tiles, and the ring's shadows from a light hanging over
# its middle (LIGHT_HEIGHT, measured from them): the mat's (a band to about 46), the top ropes' (a line at about 49) and
# the corners' (a stripe out along each corner's middle line). The new floor: one picture laid flat over +-76; the tiles
# as one even grid (the picture's clean rows with its light divided out); the light as the square's at the same share of
# the way out to the barricade and along a side; the six-sided ring's own shadows from the same light.
LIGHT_HEIGHT = 84.0
FLOOR_PICTURE = 768
SHADOW_SOFT = 0.5


def autoperiod(profile, lo, hi, step=0.05):
    """The period (in samples) at which the profile best matches itself, lo to hi."""
    n = len(profile)
    a, b = fit_line(list(enumerate(profile)))   # its slope taken out first: a falling light would hide the period
    p = [v - (a + b * i) for i, v in enumerate(profile)]
    best, best_c = lo, -2.0
    period = lo
    while period <= hi:
        num = da = db = 0.0
        for i in range(n):
            j = i + period
            k = int(j)
            if k + 1 >= n:
                break
            b = p[k] + (p[k + 1] - p[k]) * (j - k)
            num += p[i] * b
            da += p[i] * p[i]
            db += b * b
        c = num / math.sqrt(da * db) if da and db else -1.0
        if c > best_c:
            best, best_c = period, c
        period += step
    return best


class FloorPicture:
    def __init__(self, picture, floor, pieces):
        self.pic = picture
        w, h = picture.width, picture.height
        # the front trapezoid: the triangles with the outer middle vertex (0, 0, 76)
        vs = floor.vertices
        out_mid = [i for i, v in enumerate(vs) if abs(v[6]) < 0.01 and abs(v[7]) < 0.01 and v[8] > 70]
        tris = [t for t in floor.triangles() if any(i in out_mid[:1] for i in t)]
        front = [vs[i] for i in sorted({i for t in tris for i in t})]
        self.front_tris = [[vs[i] for i in t] for t in tris]
        self.out = max(v[8] for v in front)
        self.inner = min(v[8] for v in front)
        row = lambda t: (t - math.floor(t)) * h
        r_in = sum(row(v[1]) for v in front if v[8] < self.inner + 0.01) / sum(1 for v in front if v[8] < self.inner + 0.01)
        r_out = sum(row(v[1]) for v in front if v[8] > self.out - 0.01) / sum(1 for v in front if v[8] > self.out - 0.01)
        if r_out < r_in:
            raise SystemExit('the floor\'s picture is laid the other way round')
        self.r_in, self.r_out = r_in, r_out
        s_in = min(v[0] for v in front if v[8] < self.inner + 0.01)
        s_out = min(v[0] for v in front if v[8] > self.out - 0.01)
        self.s_min = lambda r: s_in + (s_out - s_in) * (r - r_in) / (r_out - r_in)
        self.rows_per_unit = (r_out - r_in) / (self.out - self.inner)
        band = [v for v in vs if MAT - 0.05 <= max(abs(v[6]), abs(v[8])) <= MAT + 0.05]
        self.band_in_light = tuple(sum(v[2 + c] for v in band) / len(band) for c in range(4))
        ring = [v for v in vs if abs(max(abs(v[6]), abs(v[8])) - self.inner) < 0.01]
        dark = [v for v in ring if v[2] < 80]   # the band's own outer edge: darker than the trapezoids' at the same place
        self.band_out_light = tuple(sum(v[2 + c] for v in dark) / len(dark) for c in range(4)) if dark else self.band_in_light
        self.r_band = min(row(v[1]) for v in band)

        def lum(c, r):
            p = picture.at(c, r)
            return (p[0] + p[1] + p[2]) / 3.0

        def f_of(c, r):
            s = (c + 0.5) / w
            return (s - 0.5) / max(1e-6, 0.5 - self.s_min(r + 0.5))
        self.f_of = f_of
        # the light along the rows (per channel, a line), from the side's middle; the rope's line and the grout left out
        rows = []
        for r in range(int(r_in + 8), h):
            cols = [c for c in range(w) if abs(f_of(c, r)) < 0.35]
            rows.append((r, [sum(picture.at(c, r)[ch] for c in cols) / len(cols) for ch in range(3)]))
        keep = rows
        for _ in range(3):
            self.light_line = [fit_line([(r, v[ch]) for r, v in keep]) for ch in range(3)]
            keep = [(r, v) for r, v in rows if v[0] > 0.9 * (self.light_line[0][0] + self.light_line[0][1] * r)]
        # across a side (by |f|), where nothing is shadowed
        bins = [[0.0, 0] for _ in range(10)]
        good = {r for r, _ in keep}
        for r in range(int(r_out - 48), h):
            if r not in good:
                continue
            b = self.light(r)
            for c in range(w):
                f = abs(f_of(c, r))
                if f <= 1.0:
                    k = min(9, int(f * 10))
                    bins[k][0] += lum(c, r) / ((b[0] + b[1] + b[2]) / 3.0)
                    bins[k][1] += 1
        across = [s / n if n else 1.0 for s, n in bins]
        self.across_table = [a / across[0] for a in across]
        # the shadow's darkness, from the mat's shadow in the band's rows
        dark_c = [0.0, 0.0, 0.0]
        n = 0
        for r in range(int(self.r_band + 2), int(r_in - 2)):
            for c in range(w):
                f = abs(f_of(c, r))
                if f < 0.35:
                    b = self.light(r)
                    a = self.across(f)
                    px = picture.at(c, r)
                    for ch in range(3):
                        dark_c[ch] += px[ch] / (b[ch] * a)
                    n += 1
        self.dark = [min(1.0, d / n) for d in dark_c]
        # the tiles: their periods across and along (the albedo's profiles), a block of whole tiles from the clean rows
        top = int(r_out - 44)
        col_profile = [sum(self.albedo(c, r)[0] for r in range(top, int(r_out))) for c in range(w)]
        self.p_col = autoperiod(col_profile, 12.0, 30.0)
        # contiguous rows past the ropes' line (the grout rows are the signal)
        row_profile = [sum(self.albedo(c, r)[0] for c in range(w) if abs(f_of(c, r)) < 0.5)
                       for r in range(int(r_in + 26), h)]
        self.p_row = autoperiod(row_profile, 28.0, 50.0)
        self.n_cols = int((w - 16) // self.p_col)
        self.c0 = (w - self.n_cols * self.p_col) / 2.0
        self.r0 = r_out - 0.5 - self.p_row
        mid_row = self.r0 + self.p_row / 2.0
        z_mid = self.inner + (mid_row - r_in) / self.rows_per_unit
        s_per_unit = (1.0 - 2.0 * self.s_min(mid_row)) / (2.0 * z_mid)
        self.tile_x = self.p_col / w / s_per_unit
        self.tile_z = self.p_row / self.rows_per_unit
        self.col_phase = (w / 2.0 - self.c0)
        # what the block's own rows and columns still rise or fall by (the light's lines are only near): taken out, so
        # side by side the blocks meet without a step
        rows_in = [r for r in range(int(self.r0) + 1, int(self.r0 + self.p_row))]
        cols_in = [c for c in range(int(self.c0) + 1, int(self.c0 + self.n_cols * self.p_col))]
        by_row = [(r, sum(self.albedo(c, r)[0] for c in cols_in) / len(cols_in)) for r in rows_in]
        by_col = [(c, sum(self.albedo(c, r)[0] for r in rows_in) / len(rows_in)) for c in cols_in]
        self.block_rows = fit_line(drop_lines(by_row))
        self.block_cols = fit_line(drop_lines(by_col))
        self.block_mean = sum(v for _, v in by_row) / len(by_row)
        self.row_phase = (mid_row - self.r0) - z_mid / self.tile_z * self.p_row
        self.pieces = pieces

    def light(self, r):
        return [max(1.0, a + b * r) for a, b in self.light_line]

    def across(self, f):
        x = min(1.0, abs(f)) * 10 - 0.5
        i = max(0, min(8, int(math.floor(x))))
        a = min(1.0, max(0.0, x - i))
        return self.across_table[i] * (1 - a) + self.across_table[i + 1] * a

    def albedo(self, c, r):
        px = self.pic.sample(c, r)
        b = self.light(r + 0.5)
        a = self.across(self.f_of(int(c) % self.pic.width, int(r) % self.pic.height))
        return [px[ch] / (b[ch] * a) for ch in range(3)]

    def tiles(self, x, z):
        span = self.n_cols * self.p_col
        c = self.c0 + (self.col_phase + x / self.tile_x * self.p_col) % span
        r = self.r0 + (self.row_phase + z / self.tile_z * self.p_row) % self.p_row
        even = self.block_mean ** 2 / ((self.block_rows[0] + self.block_rows[1] * r) *
                                       (self.block_cols[0] + self.block_cols[1] * c))
        return [v * even for v in self.albedo(c - 0.5, r - 0.5)]

    def shade(self, x, z):
        """The share of the light the ring lets through to the floor at (x, z): 0 in its shadow, 1 out of it."""
        H, soft = LIGHT_HEIGHT, SHADOW_SOFT
        dots = [dot(side_normal(k), (x, z)) for k in range(SIDES)]
        far = max(dots)
        mat = MAT * H / (H - MAT_Y)
        m = 1.0 - smooth(mat - soft, mat + soft, far)
        if m >= 1.0 or far > 80.0:
            return 1.0 - m
        for height in RING['rope heights']:
            line = ROPES * H / (H - height)
            thick = 0.4 * H / (H - height)
            reach = ROPES * math.tan(math.pi / SIDES) * H / (H - height)
            for k in range(SIDES):
                d = abs(dots[k] - line)
                if d < thick + soft:
                    along = abs(dot(side_tangent(k), (x, z)))
                    m = max(m, (1.0 - smooth(thick - soft, thick + soft, d)) * (1.0 - smooth(reach - soft, reach + soft, along)))
        r = math.hypot(x, z)
        for k in range(SIDES):
            u = angle_vec(FIRST - 360.0 / SIDES * k)
            along = x * u[0] + z * u[1]
            if along <= 0:
                continue
            across = abs(x * u[1] - z * u[0])
            for start, half, low, high in self.pieces:
                end = start * H / (H - high)
                first = start * H / (H - low) if low > 0 else start - half
                if along < first - soft or along > end + soft:
                    continue
                width = half * along / start
                m = max(m, (1.0 - smooth(width - soft, width + soft, across)) * smooth(first - soft, first + soft, along) *
                        (1.0 - smooth(end - soft, end + soft, along)))
        return 1.0 - m

    def place(self, x, z):
        """(the row of the square's picture at the same share of the way out, |f| the share along a side to a corner)."""
        k = max(range(SIDES), key=lambda i: dot(side_normal(i), (x, z)))
        flat = dot(side_normal(k), (x, z))
        f = dot(side_tangent(k), (x, z)) / max(1e-6, flat * math.tan(math.pi / SIDES))
        if flat < self.inner:
            row = self.r_band + (flat - MAT) / (self.inner - MAT) * (self.r_in - self.r_band)
        else:
            r = math.hypot(x, z)
            r_in = r * self.inner / flat
            r_out = self.out / max(abs(x), abs(z)) * r
            share = (r - r_in) / max(1e-6, r_out - r_in)
            row = self.r_in + min(1.0, max(0.0, share)) * (self.r_out - self.r_in)
        return row, abs(f)

    def colour(self, x, z):
        row, f = self.place(x, z)
        light = self.light(row)
        a = self.across(f)
        t = self.tiles(x, z)
        s = self.shade(x, z)
        return [min(255.0, max(0.0, t[ch] * light[ch] * a * (1.0 - (1.0 - self.dark[ch]) * (1.0 - s)))) for ch in range(3)]

    def make(self, size=FLOOR_PICTURE):
        rows = []
        for r in range(size):
            z = -self.out + (r + 0.5) * 2 * self.out / size
            row = bytearray()
            for i in range(size):
                x = -self.out + (i + 0.5) * 2 * self.out / size
                c = self.colour(x, z)
                row += bytes((int(c[0] + 0.5), int(c[1] + 0.5), int(c[2] + 0.5), 255))
            rows.append(row)
        return rows

    def uv(self, x, z):
        return (x + self.out) / (2 * self.out), (z + self.out) / (2 * self.out) - 1.0

    def old_light(self, x, z):
        """The square floor's baked light at (x, z) on its front trapezoid."""
        best, best_out = None, 1e30
        for tri in self.front_tris:
            w = barycentric((x, z), (tri[0][6], tri[0][8]), (tri[1][6], tri[1][8]), (tri[2][6], tri[2][8]))
            if w is None:
                continue
            o = -min(w)
            if o < best_out:
                best_out, best = o, (tri, w)
        tri, w = best
        w = [max(0.0, q) for q in w]
        total = sum(w) or 1.0
        return tuple(sum(w[i] * tri[i][2 + c] for i in range(3)) / total for c in range(4))

    def batches(self, slot, angles=24, rings=3):
        """The floor from the mat's foot to the barricade: the band (to the square's band's outer edge, its light the
        square band's) and the rest in rings between the hexagon and the square barricade, each vertex's light the
        square floor's at the same share of the way out and along a side."""
        out = []
        foot = MAT + 0.01
        vertices, strips = [], []
        for k in range(SIDES):
            for half in range(2):
                strip = []
                for along in (half * 0.5, half * 0.5 + 0.5):
                    for flat, light in ((foot, self.band_in_light), (self.inner, self.band_out_light)):
                        a, b = corner(k, flat), corner(k + 1, flat)
                        x, z = a[0] + (b[0] - a[0]) * along, a[1] + (b[1] - a[1]) * along
                        s, t = self.uv(x, z)
                        strip.append(len(vertices))
                        vertices.append((s, t) + tuple(light) + (x, 0.0, z))
                strips.append(strip)
        out.append(Batch(TEXTURED, slot, vertices, strips, []))

        def point(theta, share):
            ux, uz = angle_vec(theta)
            k = max(range(SIDES), key=lambda i: dot(side_normal(i), (ux, uz)))
            r_in = self.inner / dot(side_normal(k), (ux, uz))
            r_out = self.out / max(abs(ux), abs(uz))
            r = r_in + (r_out - r_in) * share
            x, z = ux * r, uz * r
            flat = dot(side_normal(k), (x, z))
            f = dot(side_tangent(k), (x, z)) / (flat * math.tan(math.pi / SIDES))
            zf = self.inner + share * (self.out - self.inner)
            light = self.old_light(max(-zf, min(zf, f * zf)), zf)
            s, t = self.uv(x, z)
            return (s, t) + tuple(light) + (x, 0.0, z)
        sectors = []
        for i in range(angles):
            t0, t1 = 360.0 * i / angles, 360.0 * (i + 1) / angles
            strip = []
            for j in range(rings + 1):
                share = j / rings
                strip += [point(t0, share), point(t1, share)]
            sectors.append(strip)
        per = max(1, 72 // (2 * (rings + 1)))
        for at in range(0, len(sectors), per):
            vertices, strips = [], []
            for strip in sectors[at:at + per]:
                strips.append(list(range(len(vertices), len(vertices) + len(strip))))
                vertices += strip
            out.append(Batch(TEXTURED, slot, vertices, strips, []))
        return out


def drop_lines(points):
    """The points without the grout's lines (well off the run of the rest)."""
    a, b = fit_line(points)
    return [(x, v) for x, v in points if abs(v - (a + b * x)) < 0.08 * abs(a + b * x)] or points


def fit_line(points):
    n = len(points)
    sx = sum(p[0] for p in points)
    sy = sum(p[1] for p in points)
    sxx = sum(p[0] * p[0] for p in points)
    sxy = sum(p[0] * p[1] for p in points)
    b = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    return (sy - b * sx) / n, b


def corner_shadow_casters(post, pad, turnbuckle):
    """Each corner piece as the floor's shadows need it: (its middle's distance out along the corner's middle line,
    its half width, its lowest and highest points), the six-sided ring's (as corner_pieces puts them)."""
    out = []
    for (_, vertices, _), how in ((post, 'post'), (pad, 'pad'), (turnbuckle, 'turnbuckle')):
        xs = [post_xyz(v)[0] for v in vertices]
        ys = [post_xyz(v)[1] for v in vertices]
        zs = [post_xyz(v)[2] for v in vertices]
        mx, mz = (min(xs) + max(xs)) / 2, (min(zs) + max(zs)) / 2
        flat = (abs(mx) + abs(mz)) / 2
        start = flat / math.cos(math.pi / SIDES)
        if how == 'post':
            half = (max(xs) - min(xs)) / 2
        elif how == 'pad':
            half = 0.3
        else:
            half = max(0.5, (max(xs) - min(xs)) / 2)
        out.append((start, half, min(ys), max(ys)))
    return out


# ---- The Face Club, six-sided

def is_top(b):
    return all(abs(v[7] - MAT_Y) < 0.01 and max(abs(v[6]), abs(v[8])) <= MAT + 0.05 for v in b.vertices)


def is_floor(b):
    """The floor round the ring: flat on the ground, out to the barricade, with the middle of the front side's outer edge."""
    return (all(abs(v[7]) < 0.05 for v in b.vertices) and max(max(abs(v[6]), abs(v[8])) for v in b.vertices) > 70 and
            any(abs(v[6]) < 0.01 and v[8] > 70 for v in b.vertices) and
            any(abs(max(abs(v[6]), abs(v[8])) - MAT) < 0.05 for v in b.vertices))


def is_skirt(b):
    return all(-0.05 <= v[7] <= MAT_Y + 0.05 and MAT - 0.05 <= max(abs(v[6]), abs(v[8])) <= MAT + 0.05
               for v in b.vertices) and any(v[7] < 0.01 for v in b.vertices)


def six_sided_face_club(game):
    venue = 7
    head, parts = read_scene(game.chunk(SCENE_CHUNK + 2 * venue))
    by_id = {}
    for p in parts:
        by_id.setdefault(p.id, p)
    scenery = read_mesh(by_id[0].data)
    tops = [b for b in scenery.batches if b.prog == TEXTURED and is_top(b)]
    skirts = [b for b in scenery.batches if b.prog == TEXTURED and is_skirt(b)]
    if len(tops) != 2 or len(skirts) != 1 or len({b.slot for b in tops}) != 1:
        raise SystemExit(f'The Face Club\'s ring is not as expected ({len(tops)} mat batches, {len(skirts)} side batches)')
    floors = [b for b in scenery.batches if b.prog == TEXTURED and is_floor(b)]
    if len(floors) != 1:
        raise SystemExit(f'The Face Club\'s floor round the ring is not as expected ({len(floors)} batches)')
    textures = game.chunk(TEXTURE_CHUNK + 2 * venue)
    square = SquareMat(tops)
    mat_tex0 = scenery.slots[tops[0].slot][0]
    mat = MatPicture(eight_bit_picture(textures, mat_tex0), square)
    post, pad, turnbuckle = (read_post(by_id[i].data) for i in (81, 85, 89))
    floor_tex0 = scenery.slots[floors[0].slot][0]
    floor = FloorPicture(eight_bit_picture(textures, floor_tex0), floors[0], corner_shadow_casters(post, pad, turnbuckle))
    rest = [b for b in scenery.batches if b not in tops and b not in skirts and b not in floors]
    new_mat = mat_batches(square, tops[0].slot, mat.new_uv)
    new_floor = floor.batches(floors[0].slot)
    scenery.batches = rest + new_mat + [skirt_batch(skirts[0])] + new_floor
    shadow = read_mesh(by_id[99].data)
    shadow.batches = shadow_floor(shadow, new_mat + new_floor, floor.out)
    pieces = corner_pieces(post, pad, turnbuckle)
    pictures = {mat_tex0 & 0x3FFF: (MAT_PICTURE, mat.make()), floor_tex0 & 0x3FFF: (FLOOR_PICTURE, floor.make())}
    out = []
    for p in parts:
        if 81 <= p.id <= 92:
            continue
        if p.id == 0:
            out.append(Part(0, write_mesh(scenery)))
        elif p.id == 99:
            out.append(Part(99, write_mesh(shadow)))
        else:
            out.append(p)
    out += pieces
    scene = write_scene(head, out)
    people, clips = (game.chunk(c) for c in CROWD_CHUNKS[venue])
    return (venue, scene, textures, people, clips), pictures


def write_arena(venue, scene, textures, people, clips):
    parts = [scene, textures, people or b'', clips or b'']
    offsets, at = [], 0x40
    for p in parts:
        at = (at + 15) & ~15
        offsets.append(at if p else 0)
        at += len(p)
    out = bytearray(at)
    out[0:8] = b'DJARENA1'
    struct.pack_into('<Ii', out, 8, 0x40, venue)
    struct.pack_into('<IIII', out, 0x10, offsets[0], len(parts[0]), offsets[1], len(parts[1]))
    out[0x20:0x20 + len(SERIAL)] = SERIAL
    struct.pack_into('<IIII', out, 0x30, offsets[2], len(parts[2]), offsets[3], len(parts[3]))
    for o, p in zip(offsets, parts):
        if p:
            out[o:o + len(p)] = p
    return bytes(out)


RING = {'sides': 6, 'rope distance': 35, 'mat height': 10.5, 'rope heights': [24.1, 19.3, 14.5], 'slump distance': 31,
        'apron': {'inner': 34, 'line': 38.5, 'outer': 43}, 'ring side': 41, 'outside wall': 77}

NAME = 'THE FACE CLUB SIX SIDES'
# Its Choose Stage pictures: the recomp's own when standard\\arenas\\<name>\\ has them (the name strip, the big picture
# and the thumbnail, in the game's frame); else taken by the setup from the arena's own fly-in on the player's PC: the
# camera of The Face Club's fly-in about frame 105, as its first shot zooms out from the
# DJ to the whole ring (its eye; its target and field of view three quarters of the way from frame 0's to frame 140's)
# (eye x y z, target x y z, roll, fov), and the cuts of the picture (shares of its width and height: u0 v0 u1 v1) for
# the big picture (Choose Stage shows it at 354 x 197, so the whole width at 1.8 : 1 of the 4 : 3 picture) and the
# thumbnail (square on screen, round the ring)
PICTURE_CAMERA = [-54.7, 57.0, 219.2, 9.0, 44.0, -7.0, 1.0, 27.0]
PICTURE_CUTS = [[0.0, 0.259, 1.0, 1.0], [0.12, 0.173, 0.74, 1.0]]
SHIPPED = os.path.join(REPO, 'standard', 'arenas')   # what the recomp brings of its own for each (the name strip)


def standard_root(game):
    return os.path.join(game, 'standard', 'arenas')


def make(game_folder, out=None):
    """The arena's folder made from the game's files (its model, textures, arena.json, the shipped name strip); its path."""
    out = out or os.path.join(standard_root(game_folder), NAME)
    game = Game(game_folder)
    parts, pictures = six_sided_face_club(game)
    data = write_arena(*parts)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'model.arena'), 'wb') as f:
        f.write(data)
    folder = os.path.join(out, 'textures')
    os.makedirs(folder, exist_ok=True)
    for address, (size, rows) in pictures.items():
        with open(os.path.join(folder, f'@{address:04X}.png'), 'wb') as f:
            f.write(png(size, size, rows))
    for f in shipped_pictures():
        shutil.copyfile(os.path.join(SHIPPED, NAME, f), os.path.join(out, f))
    write_arena_json(out)
    return out


def shipped_pictures():
    """Its Choose Stage pictures the recomp brings (standard\\arenas\\<name>\\): the name strip, and the big picture and
    thumbnail when they are there (then the setup takes none)."""
    folder = os.path.join(SHIPPED, NAME)
    return [f for f in ('name.png', 'picture.png', 'thumbnail.png') if os.path.isfile(os.path.join(folder, f))]


def pictures_shipped():
    return {'picture.png', 'thumbnail.png'} <= set(shipped_pictures())


def write_arena_json(folder):
    """arena.json: its name and ring; until its own pictures are taken, The Face Club's in their place."""
    arena = {'name': NAME, 'ring': RING}
    have = all(os.path.isfile(os.path.join(folder, f)) for f in ('picture.png', 'thumbnail.png'))
    if not have:
        arena['from the game'] = {'picture': 'The Face Club', 'thumbnail': 'The Face Club'}
    if not os.path.isfile(os.path.join(folder, 'name.png')):
        arena.setdefault('from the game', {})['name'] = 'The Face Club'
    with open(os.path.join(folder, 'arena.json'), 'w', encoding='utf-8') as f:
        json.dump(arena, f, indent=2)


def picture_setting(folder):
    """VENDETTA_STAGE_PICTURES for the game: the folder, the camera, the two cuts."""
    numbers = lambda v: ','.join('%g' % x for x in v)
    return '|'.join([folder, numbers(PICTURE_CAMERA), numbers(PICTURE_CUTS[0]), numbers(PICTURE_CUTS[1])])


def picture_match(folder):
    """The test match the picture run plays: a 1-on-1 on this arena (its Choose Stage place: 12 on, in the standard
    arenas' order, the first new places with no mods), the computer on both sides, no movies."""
    names = sorted(n for n in os.listdir(os.path.dirname(folder))
                   if os.path.isfile(os.path.join(os.path.dirname(folder), n, 'model.arena')))
    place = 12 + names.index(os.path.basename(folder))
    return {'match': '1-on-1', 'arena': place, 'skip movies': True,
            'fighters': [{'record': 0, 'attire': 0, 'control': 'cpu'}, {'record': 9, 'attire': 0, 'control': 'cpu'}]}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', default=os.path.join(REPO, 'game'))
    ap.add_argument('--out', help='the folder to write (default: the game folder\'s standard\\arenas\\<its name>)')
    ap.add_argument('--name', default=None, help='arena.json\'s name (a test mod\'s)')
    a = ap.parse_args()
    out = make(a.game, a.out)
    if a.name:
        path = os.path.join(out, 'arena.json')
        arena = json.load(open(path, encoding='utf-8'))
        arena['name'] = a.name
        json.dump(arena, open(path, 'w', encoding='utf-8'), indent=2)
    print(f'made {out}')


if __name__ == '__main__':
    sys.exit(main())
