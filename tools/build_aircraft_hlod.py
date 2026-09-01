#!/usr/bin/env python3
"""Build flight-only HLOD tiles from the user's installed GTA SA data.

This tool deliberately does not copy DFFs into the source tree.  It reads the
retail map IPL/IDE files and War Drum native DFFs from a local data split,
applies the IPL transforms offline and emits a small, renderer-independent
tile pack.  Authored parent LODs form the city layer; rootless road/land models
fill the terrain layer that GTA's camera-wedge admission can otherwise omit.

The first pack format bakes a representative stock-texture colour into every
vertex.  Missing thumbnails receive a conservative semantic fallback and the
baked colours receive a mild luminance-preserving grade before welding.
Transparent materials are omitted.  That makes it useful as an
always-resident opaque safety layer without repeating the old collision-box
proxy.  Texture names remain in the JSON report so a later atlas pass can
replace the colour-only material without changing tile selection.
"""

from __future__ import annotations

import argparse
import json
import math
import mmap
import re
import struct
import sys
import zipfile
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

sys.stdout.reconfigure(errors="replace")

SECTOR = 2048
RW_STRUCT = 0x01
RW_STRING = 0x02
RW_EXTENSION = 0x03
RW_TEXTURE = 0x06
RW_MATERIAL = 0x07
RW_MATLIST = 0x08
RW_GEOMETRY = 0x0F
RW_CLUMP = 0x10
RW_GEOMETRY_LIST = 0x1A
RW_BIN_MESH = 0x50E
RW_NATIVE_DATA = 0x510

GROUND_TOKENS = (
    "road", "land", "ground", "coast", "beach", "seabed", "river",
    "canal", "pier", "dock", "bridge", "freeway", "junction", "rail",
    "runway", "taxiway", "hill", "mount", "desert", "cliff", "under",
)
ALPHA_TOKENS = (
    "tree", "plant", "grass", "bush", "weed", "fern", "palm", "fence",
    "wire", "glass", "window", "banner", "flag", "cloth", "net", "sign",
)

# The stock mobile texture database does not contain a thumbnail for every DFF
# texture name.  Falling every miss back to the same neutral grey made the
# position-welded flight HLOD lose most of San Andreas' regional colour.  These
# ordered profiles provide a conservative fallback and a mild post-bake grade;
# they never change alpha classification, geometry or the weld key.
HLOD_COLOUR_PROFILES = (
    (("water", "river", "canal", "seabed"),
     (145, 165, 178), 0.94, 1.18, (0.92, 1.02, 1.10)),
    (("grass", "forestfloor", "lawn", "moss", "green"),
     (157, 175, 132), 0.94, 1.24, (0.94, 1.07, 0.86)),
    (("tree", "ivy", "hedge", "plant", "bush", "palm", "weed", "fern"),
     (160, 170, 145), 0.98, 1.14, (0.97, 1.04, 0.92)),
    (("wood", "plank", "log"),
     (174, 157, 132), 0.94, 1.18, (1.06, 1.00, 0.88)),
    (("dirt", "sand", "desert", "rock", "cliff", "mount", "hill",
      "beach", "coast", "ground", "stone", "trail", "gravel", "land"),
     (181, 164, 137), 0.94, 1.20, (1.07, 1.00, 0.88)),
    (("roof", "tile", "shingle"),
     (178, 161, 153), 0.94, 1.18, (1.05, 0.98, 0.94)),
    (("road", "freeway", "runway", "taxi", "tarmac", "asphalt", "pave",
      "carpark", "rail", "traintrax", "junction", "drain", "concrete"),
     (158, 162, 168), 0.94, 1.06, (0.98, 0.99, 1.03)),
    (("metal", "steel", "corrug", "iron"),
     (160, 166, 174), 0.94, 1.10, (0.97, 1.00, 1.04)),
    (("wall", "house", "hotel", "office", "shop", "block", "blok",
      "brick", "motel", "project", "warehouse", "factory", "church"),
     (176, 168, 160), 0.94, 1.14, (1.02, 1.00, 0.97)),
)
HLOD_DEFAULT_COLOUR_PROFILE = (
    (176, 176, 176), 0.94, 1.10, (1.0, 1.0, 1.0))


@dataclass(frozen=True)
class Chunk:
    kind: int
    body: int
    end: int
    version: int


@dataclass
class Material:
    rgba: tuple[int, int, int, int]
    texture: str
    texture_rgba: tuple[int, int, int, int]
    transparent: bool


@dataclass
class Geometry:
    vertices: list[tuple[float, float, float]]
    prelight: list[tuple[int, int, int, int]]
    meshes: list[tuple[int, list[int]]]
    materials: list[Material]


@dataclass
class Instance:
    model_id: int
    model_name: str
    interior: int
    pos: tuple[float, float, float]
    quat: tuple[float, float, float, float]
    lod_index: int
    source: str
    source_index: int
    layer: str = ""


def iter_chunks(data, start: int, end: int):
    pos = start
    while pos + 12 <= end:
        kind, size, version = struct.unpack_from("<III", data, pos)
        body = pos + 12
        chunk_end = body + size
        if chunk_end > end:
            break
        yield Chunk(kind, body, chunk_end, version)
        pos = chunk_end


def first_chunk(data, start: int, end: int, kind: int):
    return next((c for c in iter_chunks(data, start, end) if c.kind == kind), None)


class ImgArchive:
    def __init__(self, path: Path):
        self.path = path
        self.file = path.open("rb")
        self.data = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        if self.data[:4] != b"VER2":
            raise ValueError(f"not a VER2 IMG: {path}")
        count = struct.unpack_from("<I", self.data, 4)[0]
        self.entries = {}
        for i in range(count):
            off = 8 + i * 32
            sector, stream_sectors, archive_sectors = struct.unpack_from(
                "<IHH", self.data, off)
            raw = self.data[off + 8:off + 32].partition(bytes((0,)))[0]
            name = raw.decode("ascii", "replace").lower()
            sectors = archive_sectors or stream_sectors
            self.entries[name] = (sector * SECTOR, sectors * SECTOR)

    def read(self, name: str):
        item = self.entries.get(name.lower())
        if not item:
            return None
        off, size = item
        return memoryview(self.data)[off:off + size]

    def close(self):
        self.data.close()
        self.file.close()


def expand4(x):
    return (x << 4) | x


def expand5(x):
    return (x << 3) | (x >> 2)


def clamp8(x):
    return max(0, min(255, x))


def clamp01(x):
    return max(0.0, min(1.0, x))


def hlod_colour_profile(texture_name):
    name = texture_name.lower()
    for tokens, anchor, gamma, saturation, tint in HLOD_COLOUR_PROFILES:
        if any(token in name for token in tokens):
            return anchor, gamma, saturation, tint
    return HLOD_DEFAULT_COLOUR_PROFILE


def sign3(x):
    return x - 8 if x & 4 else x


ETC_MODIFIERS = (
    (2, 8, -2, -8), (5, 17, -5, -17), (9, 29, -9, -29),
    (13, 42, -13, -42), (18, 60, -18, -60), (24, 80, -24, -80),
    (33, 106, -33, -106), (47, 183, -47, -183),
)


def etc1_average(raw: bytes):
    """Return the mean RGB of an ETC1 image.  Selector orientation does not
    affect the mean, which is all the HLOD palette needs."""
    total = [0, 0, 0]
    count = 0
    for off in range(0, len(raw) - 7, 8):
        block = int.from_bytes(raw[off:off + 8], "big")
        hi, lo = block >> 32, block & 0xFFFFFFFF
        if hi & 2:
            r1, g1, b1 = (hi >> 27) & 31, (hi >> 19) & 31, (hi >> 11) & 31
            r2, g2, b2 = r1 + sign3((hi >> 24) & 7), \
                         g1 + sign3((hi >> 16) & 7), \
                         b1 + sign3((hi >> 8) & 7)
            bases = ((expand5(max(0, min(31, r1))),
                      expand5(max(0, min(31, g1))),
                      expand5(max(0, min(31, b1)))),
                     (expand5(max(0, min(31, r2))),
                      expand5(max(0, min(31, g2))),
                      expand5(max(0, min(31, b2)))))
        else:
            bases = ((expand4((hi >> 28) & 15), expand4((hi >> 20) & 15),
                      expand4((hi >> 12) & 15)),
                     (expand4((hi >> 24) & 15), expand4((hi >> 16) & 15),
                      expand4((hi >> 8) & 15)))
        tables = ((hi >> 5) & 7, (hi >> 2) & 7)
        flip = hi & 1
        for y in range(4):
            for x in range(4):
                bit = x * 4 + y
                selector = ((lo >> (bit + 16)) & 1) * 2 + ((lo >> bit) & 1)
                half = (1 if y >= 2 else 0) if flip else (1 if x >= 2 else 0)
                mod = ETC_MODIFIERS[tables[half]][selector]
                for c in range(3):
                    total[c] += clamp8(bases[half][c] + mod)
                count += 1
    if not count:
        return (128, 128, 128, 255)
    return tuple(round(v / count) for v in total) + (255,)


def raw_4444_average(raw: bytes):
    total = [0, 0, 0, 0]
    count = 0
    for off in range(0, len(raw) - 1, 2):
        p = struct.unpack_from("<H", raw, off)[0]
        # GL_UNSIGNED_SHORT_4_4_4_4 with the mobile BGRA upload path.
        vals = (expand4((p >> 12) & 15), expand4((p >> 8) & 15),
                expand4((p >> 4) & 15), expand4(p & 15))
        for i, value in enumerate(vals):
            total[i] += value
        count += 1
    if not count:
        return (128, 128, 128, 255)
    return tuple(round(v / count) for v in total)


def load_texture_palette(apk: Path):
    palette = {}
    alpha_names = set()
    with zipfile.ZipFile(apk) as zf:
        base = "assets/texdb/gta3/gta3"
        lines = zf.read(base + ".txt").decode("utf-8", "replace").splitlines()
        toc_data = zf.read(base + ".etc.toc")
        toc = struct.unpack("<%dI" % (len(toc_data) // 4), toc_data)
        thumbs = zf.read(base + ".etc.tmb")
        thumb_pos = 0
        for line_index, line in enumerate(lines[1:], 1):
            match = re.match(r'"([^"]+)"', line)
            if not match:
                continue
            name = match.group(1).lower()
            if "alphamode=" in line:
                alpha_names.add(name)
            if line_index >= len(toc) or toc[line_index] == 0xFFFFFFFF:
                continue
            if thumb_pos + 12 > len(thumbs):
                break
            fmt, width, height, size = struct.unpack_from("<IHHI", thumbs, thumb_pos)
            payload = thumbs[thumb_pos + 12:thumb_pos + 12 + size]
            thumb_pos += 12 + size
            pixels = payload[4:] if len(payload) >= 4 else payload
            if width == 1 and height == 1 and len(pixels) >= 4:
                colour = tuple(pixels[:4])
            elif (fmt >> 16) == 0x8D64:
                colour = etc1_average(pixels)
            elif (fmt >> 16) == 0x8033:
                colour = raw_4444_average(pixels)
            else:
                colour = (128, 128, 128, 255)
            palette[name] = colour
    return palette, alpha_names


def texture_name(data, texture: Chunk):
    for child in iter_chunks(data, texture.body, texture.end):
        if child.kind == RW_STRING:
            raw = bytes(data[child.body:child.end]).partition(bytes((0,)))[0]
            return raw.decode("cp1251", "replace").strip()
    return ""


def parse_material(data, chunk: Chunk, palette, alpha_names):
    struct_chunk = first_chunk(data, chunk.body, chunk.end, RW_STRUCT)
    rgba = (255, 255, 255, 255)
    textured = False
    if struct_chunk and struct_chunk.end - struct_chunk.body >= 16:
        rgba = tuple(data[struct_chunk.body + 4:struct_chunk.body + 8])
        textured = struct.unpack_from("<i", data, struct_chunk.body + 12)[0] != 0
    tex = ""
    if textured:
        tc = first_chunk(data, chunk.body, chunk.end, RW_TEXTURE)
        if tc:
            tex = texture_name(data, tc)
    tex_rgba = palette.get(tex.lower())
    if tex_rgba is None:
        # Preserve the old opaque fallback contract while recovering a small
        # amount of regional identity for textures without mobile thumbnails.
        anchor, _, _, _ = hlod_colour_profile(tex)
        tex_rgba = anchor + (255,)
    transparent = rgba[3] < 245 or tex_rgba[3] < 220 or tex.lower() in alpha_names
    return Material(rgba, tex, tex_rgba, transparent)


def parse_material_list(data, chunk: Chunk, palette, alpha_names):
    children = list(iter_chunks(data, chunk.body, chunk.end))
    info = next((c for c in children if c.kind == RW_STRUCT), None)
    if not info or info.end - info.body < 4:
        return []
    count = struct.unpack_from("<i", data, info.body)[0]
    refs = list(struct.unpack_from("<%di" % count, data, info.body + 4)) if count else []
    uniques = iter(c for c in children if c.kind == RW_MATERIAL)
    materials = []
    for index in range(count):
        ref = refs[index]
        if ref >= 0 and ref < len(materials):
            materials.append(materials[ref])
        else:
            material_chunk = next(uniques, None)
            materials.append(parse_material(data, material_chunk, palette, alpha_names)
                             if material_chunk else Material((255,) * 4, "",
                                                             (176, 176, 176, 255), False))
    return materials


def unpack_component(data, off, kind, normalized, scale):
    if kind == 0:
        return struct.unpack_from("<f", data, off)[0]
    formats = {1: ("b", 127.0), 2: ("B", 255.0),
               3: ("h", 32767.0), 4: ("H", 65535.0)}
    fmt, norm = formats[kind]
    value = struct.unpack_from("<" + fmt, data, off)[0]
    return value / (norm if normalized else scale)


def component_size(kind):
    return (4, 1, 1, 2, 2)[kind]


def parse_geometry(data, chunk: Chunk, palette, alpha_names):
    children = list(iter_chunks(data, chunk.body, chunk.end))
    info = next((c for c in children if c.kind == RW_STRUCT), None)
    matlist = next((c for c in children if c.kind == RW_MATLIST), None)
    extension = next((c for c in children if c.kind == RW_EXTENSION), None)
    if not info or info.end - info.body < 16 or not extension:
        raise ValueError("incomplete geometry")
    flags, _, num_vertices, _ = struct.unpack_from("<4I", data, info.body)
    materials = parse_material_list(data, matlist, palette, alpha_names) if matlist else []
    mesh_chunk = first_chunk(data, extension.body, extension.end, RW_BIN_MESH)
    native_chunk = first_chunk(data, extension.body, extension.end, RW_NATIVE_DATA)
    if not mesh_chunk or not native_chunk or not (flags & 0x01000000):
        raise ValueError("geometry is not War Drum native")

    mesh_flags, num_meshes, _ = struct.unpack_from("<III", data, mesh_chunk.body)
    mesh_pos = mesh_chunk.body + 12
    meshes = []
    for _ in range(num_meshes):
        count, material_index = struct.unpack_from("<Ii", data, mesh_pos)
        mesh_pos += 8
        indices = list(struct.unpack_from("<%dH" % count, data, mesh_pos))
        mesh_pos += count * 2
        if mesh_flags == 1:
            triangles = []
            for i in range(2, len(indices)):
                a, b, c = indices[i - 2:i + 1]
                if i & 1:
                    a, b = b, a
                if a != b and b != c and a != c:
                    triangles.extend((a, b, c))
            indices = triangles
        meshes.append((material_index, indices))

    pos = native_chunk.body
    num_attribs = struct.unpack_from("<I", data, pos)[0]
    pos += 4
    attribs = []
    for _ in range(num_attribs):
        attribs.append(struct.unpack_from("<IiIIII", data, pos))
        pos += 24
    vertex_base = pos
    vertices = [(0.0, 0.0, 0.0)] * num_vertices
    prelight = [(255, 255, 255, 255)] * num_vertices
    for index, kind, normalized, size, stride, offset in attribs:
        if kind < 0 or kind > 4:
            continue
        if index not in (0, 3):
            continue
        scale = 1.0
        step = component_size(kind)
        values = []
        for vertex in range(num_vertices):
            base = vertex_base + offset + vertex * stride
            values.append(tuple(unpack_component(data, base + c * step, kind,
                                                 bool(normalized), scale)
                                for c in range(size)))
        if index == 0:
            vertices = [tuple(v[:3]) for v in values]
        elif index == 3:
            prelight = [tuple(clamp8(round(c * 255.0)) for c in v[:4])
                        for v in values]
    return Geometry(vertices, prelight, meshes, materials)


def parse_dff(data, palette, alpha_names):
    root = first_chunk(data, 0, len(data), RW_CLUMP)
    if not root:
        raise ValueError("missing clump")
    gl = first_chunk(data, root.body, root.end, RW_GEOMETRY_LIST)
    if not gl:
        raise ValueError("missing geometry list")
    return [parse_geometry(data, c, palette, alpha_names)
            for c in iter_chunks(data, gl.body, gl.end) if c.kind == RW_GEOMETRY]


def parse_ide(text: str):
    models = {}
    section = ""
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        low = line.lower()
        if low in ("objs", "tobj"):
            section = low
            continue
        if low == "end":
            section = ""
            continue
        if section not in ("objs", "tobj"):
            continue
        parts = [x.strip() for x in line.split(",")]
        try:
            model_id = int(parts[0])
        except (ValueError, IndexError):
            continue
        if len(parts) >= 3:
            models[model_id] = {"name": parts[1], "txd": parts[2]}
    return models


def parse_ipl(text: str, source: str):
    instances = []
    section = ""
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        low = line.lower()
        if low == "inst":
            section = low
            continue
        if low == "end":
            section = ""
            continue
        if section != "inst":
            continue
        parts = [x.strip() for x in line.split(",")]
        if len(parts) < 11:
            continue
        try:
            instances.append(Instance(
                int(parts[0]), parts[1], int(parts[2]),
                tuple(float(x) for x in parts[3:6]),
                tuple(float(x) for x in parts[6:10]),
                int(parts[10]), source, len(instances)))
        except ValueError:
            continue
    return instances


def load_map(apk: Path):
    models = {}
    files = []
    with zipfile.ZipFile(apk) as zf:
        for name in zf.namelist():
            low = name.lower()
            if low.endswith(".ide") and "/interior/" not in low:
                models.update(parse_ide(zf.read(name).decode("cp1252", "replace")))
        for name in zf.namelist():
            low = name.lower()
            if low.endswith(".ipl") and "/interior/" not in low:
                items = parse_ipl(zf.read(name).decode("cp1252", "replace"), name)
                if items:
                    files.append(items)
    return models, files


def select_instances(files, center, radius):
    selected = []
    radius2 = radius * radius
    for items in files:
        parent_indices = {x.lod_index for x in items if x.lod_index >= 0}
        for item in items:
            if item.interior != 0:
                continue
            dx, dy = item.pos[0] - center[0], item.pos[1] - center[1]
            if dx * dx + dy * dy > radius2:
                continue
            low = item.model_name.lower()
            if any(token in low for token in ALPHA_TOKENS):
                continue
            if item.source_index in parent_indices:
                item.layer = "authored_lod"
            elif item.lod_index < 0:
                # Geometry size is checked after native DFF decode.  Name-only
                # filtering missed many anonymous land blocks and was exactly
                # the kind of heuristic that left blue holes in earlier work.
                item.layer = ("ground_root" if any(token in low for token in GROUND_TOKENS)
                              else "ordinary_root")
            else:
                continue
            selected.append(item)
    return selected


def rotate_quaternion(v, q):
    x, y, z = v
    qx, qy, qz, qw = q
    # v' = v + 2*q.xyz cross (q.xyz cross v + qw*v)
    tx, ty, tz = 2 * (qy * z - qz * y), 2 * (qz * x - qx * z), \
                 2 * (qx * y - qy * x)
    return (x + qw * tx + qy * tz - qz * ty,
            y + qw * ty + qz * tx - qx * tz,
            z + qw * tz + qx * ty - qy * tx)


def bake_colour(material: Material, prelight):
    baked = tuple(clamp8(round(material.rgba[i] * material.texture_rgba[i] *
                               prelight[i] / (255.0 * 255.0)))
                  for i in range(3))
    _, gamma, saturation, tint = hlod_colour_profile(material.texture)
    linear = [(channel / 255.0) ** gamma for channel in baked]
    luma = 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]
    saturated = [clamp01(luma + saturation * (channel - luma))
                 for channel in linear]
    saturated_luma = (0.2126 * saturated[0] + 0.7152 * saturated[1] +
                      0.0722 * saturated[2])
    tinted = [saturated[i] * tint[i] for i in range(3)]
    tinted_luma = 0.2126 * tinted[0] + 0.7152 * tinted[1] + 0.0722 * tinted[2]
    if tinted_luma > 1.0e-8:
        tinted = [channel * saturated_luma / tinted_luma for channel in tinted]
    return tuple(clamp8(round(255.0 * clamp01(channel)))
                 for channel in tinted) + (255,)


def build_tiles(archive, selected, tile_size, palette, alpha_names):
    model_cache = {}
    errors = Counter()
    tiles = defaultdict(lambda: {"vertices": [], "indices": [], "layers": Counter(),
                                 "models": Counter(), "textures": Counter()})
    for ordinal, item in enumerate(selected, 1):
        name = item.model_name.lower() + ".dff"
        if name not in model_cache:
            blob = archive.read(name)
            if blob is None:
                model_cache[name] = None
                errors["missing_dff"] += 1
            else:
                try:
                    model_cache[name] = parse_dff(blob, palette, alpha_names)
                except Exception as exc:  # keep the report useful across odd plugins
                    model_cache[name] = None
                    errors[type(exc).__name__ + ":" + str(exc)] += 1
        geometries = model_cache[name]
        if not geometries:
            continue
        if item.layer == "ordinary_root":
            all_vertices = [v for geometry in geometries for v in geometry.vertices]
            if not all_vertices:
                continue
            extent_x = max(v[0] for v in all_vertices) - min(v[0] for v in all_vertices)
            extent_y = max(v[1] for v in all_vertices) - min(v[1] for v in all_vertices)
            # Keep substantial map pieces and buildings, not lamps, bins and
            # other rootless props.  A 14 m span is smaller than one city lot
            # but large enough to close unnamed terrain seams.
            if max(extent_x, extent_y) < 14.0:
                continue
            item.layer = "large_root"
        tile_key = (math.floor(item.pos[0] / tile_size),
                    math.floor(item.pos[1] / tile_size))
        tile = tiles[tile_key]
        tile["layers"][item.layer] += 1
        tile["models"][item.model_name] += 1
        for geometry in geometries:
            transformed = []
            for vertex in geometry.vertices:
                r = rotate_quaternion(vertex, item.quat)
                transformed.append((r[0] + item.pos[0], r[1] + item.pos[1],
                                    r[2] + item.pos[2]))
            for mat_index, indices in geometry.meshes:
                material = geometry.materials[mat_index] if 0 <= mat_index < len(geometry.materials) \
                    else Material((255,) * 4, "", (176, 176, 176, 255), False)
                if material.transparent or not indices:
                    continue
                base = len(tile["vertices"])
                # De-index per material so its representative colour is exact.
                remap = {}
                for index in indices:
                    if index >= len(transformed):
                        continue
                    if index not in remap:
                        remap[index] = len(tile["vertices"]) - base
                        colour = bake_colour(material, geometry.prelight[index])
                        tile["vertices"].append(transformed[index] + colour)
                    tile["indices"].append(base + remap[index])
                tile["textures"][material.texture or "<none>"] += len(indices) // 3
        if ordinal % 250 == 0:
            print(f"decoded {ordinal}/{len(selected)} instances", flush=True)
    return tiles, model_cache, errors


def cluster_tiles(tiles, step, colour_step):
    if step <= 0.0:
        return
    for tile in tiles.values():
        old_vertices, old_indices = tile["vertices"], tile["indices"]
        if not old_indices:
            continue
        accum = []
        lookup = {}
        remap = [0] * len(old_vertices)
        for old_index, vertex in enumerate(old_vertices):
            spatial_key = (round(vertex[0] / step), round(vertex[1] / step),
                           round(vertex[2] / step))
            # Far HLOD shape matters more than preserving every material seam.
            # Keeping RGB in the weld key left nearly every source triangle
            # disconnected and produced a half-million-triangle "HLOD".  A
            # non-positive colour step deliberately welds by position only and
            # averages the contributing stock colours at the merged vertex.
            colour_key = (() if colour_step <= 0 else
                          tuple(value // colour_step for value in vertex[3:6]))
            key = spatial_key + colour_key
            new_index = lookup.get(key)
            if new_index is None:
                new_index = len(accum)
                lookup[key] = new_index
                accum.append([vertex[0], vertex[1], vertex[2],
                              vertex[3], vertex[4], vertex[5], 1])
            else:
                dst = accum[new_index]
                for i in range(6):
                    dst[i] += vertex[i]
                dst[6] += 1
            remap[old_index] = new_index
        indices = []
        for i in range(0, len(old_indices) - 2, 3):
            a, b, c = (remap[old_indices[i + j]] for j in range(3))
            if a != b and b != c and a != c:
                indices.extend((a, b, c))
        used = sorted(set(indices))
        compact = {old: new for new, old in enumerate(used)}
        vertices = []
        for old in used:
            value = accum[old]
            count = value[6]
            vertices.append(tuple(value[i] / count for i in range(3)) +
                            tuple(clamp8(round(value[i] / count)) for i in range(3, 6)) +
                            (255,))
        tile["vertices"] = vertices
        tile["indices"] = [compact[x] for x in indices]


def write_pack(path: Path, tiles, tile_size):
    path.parent.mkdir(parents=True, exist_ok=True)
    usable = [(key, tile) for key, tile in sorted(tiles.items()) if tile["indices"]]
    with path.open("wb") as out:
        out.write(struct.pack("<8sIfI", b"SAVRHLD2", 2, tile_size, len(usable)))
        for (tx, ty), tile in usable:
            verts, inds = tile["vertices"], tile["indices"]
            if len(verts) > 65535:
                raise ValueError(f"tile {tx},{ty} has {len(verts)} vertices; "
                                 "reduce tile size or increase clustering")
            xs, ys, zs = [v[0] for v in verts], [v[1] for v in verts], [v[2] for v in verts]
            out.write(struct.pack("<ii6fII", tx, ty, min(xs), min(ys), min(zs),
                                  max(xs), max(ys), max(zs), len(verts), len(inds)))
            for x, y, z, r, g, b, a in verts:
                out.write(struct.pack("<3f4B", x, y, z, r, g, b, a))
            out.write(struct.pack("<%dH" % len(inds), *inds))
    return len(usable)


def write_preview(path: Path, tiles, center, radius, size=1600):
    """Rasterize a diagnostic top-down image.  This is intentionally a build
    artifact, not a runtime texture; it catches missing map strips, bad native
    vertex decoding and wildly wrong palette values before a headset build."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return False
    image = Image.new("RGB", (size, size), (35, 80, 105))
    draw = ImageDraw.Draw(image)
    scale = size / (radius * 2.0)

    def screen(vertex):
        return ((vertex[0] - (center[0] - radius)) * scale,
                size - (vertex[1] - (center[1] - radius)) * scale)

    triangles = []
    for tile in tiles.values():
        vertices, indices = tile["vertices"], tile["indices"]
        for i in range(0, len(indices) - 2, 3):
            a, b, c = (vertices[indices[i + j]] for j in range(3))
            z = (a[2] + b[2] + c[2]) / 3.0
            colour = tuple(round((a[3 + k] + b[3 + k] + c[3 + k]) / 3.0)
                           for k in range(3))
            triangles.append((z, (screen(a), screen(b), screen(c)), colour))
    # Terrain first, roofs last.  The preview has no depth buffer.
    triangles.sort(key=lambda x: x[0])
    for _, points, colour in triangles:
        draw.polygon(points, fill=colour)
    for offset in range(-int(radius // 256) - 1, int(radius // 256) + 2):
        x = (center[0] + offset * 256 - (center[0] - radius)) * scale
        y = size - (center[1] + offset * 256 - (center[1] - radius)) * scale
        draw.line((x, 0, x, size), fill=(255, 255, 255), width=1)
        draw.line((0, y, size, y), fill=(255, 255, 255), width=1)
    image.save(path)
    return True


def write_report(path: Path, args, selected, tiles, cache, errors, palette):
    path.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "source_apk": str(args.apk), "source_img": str(args.img),
        "center": args.center, "radius": args.radius, "tile_size": args.tile_size,
        "cluster_step": args.cluster_step,
        "cluster_colour_step": args.cluster_colour_step,
        "colour_grade": "semantic-thumbnail-gamma-v1",
        "selected_instances": len(selected),
        "selected_layers": Counter(x.layer for x in selected),
        "selected_models": len({x.model_name.lower() for x in selected}),
        "decoded_models": sum(v is not None for v in cache.values()),
        "decode_errors": errors,
        "palette_entries": len(palette),
        "tiles": len(tiles),
        "vertices": sum(len(t["vertices"]) for t in tiles.values()),
        "triangles": sum(len(t["indices"]) // 3 for t in tiles.values()),
        "tile_summary": {
            f"{k[0]},{k[1]}": {
                "instances": sum(t["layers"].values()),
                "layers": t["layers"], "vertices": len(t["vertices"]),
                "triangles": len(t["indices"]) // 3,
                "top_models": t["models"].most_common(12),
                "top_textures": t["textures"].most_common(12),
            } for k, t in sorted(tiles.items())
        },
    }
    path.write_text(json.dumps(report, indent=2, ensure_ascii=False, default=dict),
                    encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apk", type=Path, required=True,
                        help="retail split_data_main.apk containing map/texdb data")
    parser.add_argument("--img", type=Path, required=True,
                        help="extracted retail assets/texdb/gta3.img")
    parser.add_argument("--out", type=Path, default=Path("build/aircraft-hlod"))
    parser.add_argument("--center", type=float, nargs=2, default=(1300.0, -1250.0))
    parser.add_argument("--radius", type=float, default=2400.0)
    parser.add_argument("--tile-size", type=float, default=256.0)
    parser.add_argument("--cluster-step", type=float, default=1.0,
                        help="vertex clustering grid in metres (0 disables)")
    parser.add_argument(
        "--cluster-colour-step", type=int, default=16,
        help=("RGB quantisation used in the weld key; 0 welds by position "
              "only and averages colours for a genuinely cheap far mesh"))
    args = parser.parse_args()

    print("loading stock texture thumbnails...")
    palette, alpha_names = load_texture_palette(args.apk)
    print(f"palette={len(palette)} alpha={len(alpha_names)}")
    print("loading outdoor IDE/IPL map...")
    _, files = load_map(args.apk)
    selected = select_instances(files, args.center, args.radius)
    print(f"selected={len(selected)} instances")

    archive = ImgArchive(args.img)
    try:
        tiles, cache, errors = build_tiles(archive, selected, args.tile_size,
                                           palette, alpha_names)
        cluster_tiles(tiles, args.cluster_step, args.cluster_colour_step)
        count = write_pack(args.out / "aircraft.hlod", tiles, args.tile_size)
        write_preview(args.out / "coverage-preview.png", tiles, args.center,
                      args.radius)
        report = write_report(args.out / "report.json", args, selected, tiles,
                              cache, errors, palette)
    finally:
        # Parsed memoryviews are short-lived inside build_tiles.
        archive.close()
    print(f"wrote {count} tiles, {report['triangles']} triangles, "
          f"{(args.out / 'aircraft.hlod').stat().st_size / 1048576:.2f} MiB")
    print(f"report: {args.out / 'report.json'}")


if __name__ == "__main__":
    main()
