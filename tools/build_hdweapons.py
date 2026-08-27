#!/usr/bin/env python3
"""Build the HD-weapons payload from a downloaded model pack.

Input: a pack folder containing weapon .dff files and replacement .png
textures (the LibertyCity "Original HD Weapons" layout works as-is).
Output: a device payload directory:

    hdweapons/hdweapons.img              VER2 image with the weapon DFFs
    texdb/hdweapons/hdweapons.txt   text-format texture database listing
    texdb/hdweapons/src/<name>.png  loose PNG per texture

The runtime registers the image via CStreaming::AddImageToList and the
texture folder via TextureDatabaseRuntime::Load(format 0), so nothing in
the stock APK is modified.  Texture names are extracted from each DFF's
material sections and matched case-insensitively against the pack's PNG
files; the PNG is written under the exact name the DFF references, since
the engine's texture lookup hashes the name case-sensitively.
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

sys.stdout.reconfigure(errors="replace")

SECTOR = 2048

# RenderWare section ids that contain child sections.
RW_CONTAINERS = {0x03, 0x06, 0x07, 0x08, 0x0E, 0x0F, 0x10, 0x14, 0x1A}
RW_TEXTURE = 0x06
RW_STRING = 0x02


def walk_rw(data, start, end, in_texture, out_names):
    pos = start
    while pos + 12 <= end:
        sec_type, sec_size, _ = struct.unpack_from("<III", data, pos)
        body = pos + 12
        if body + sec_size > end:
            break
        if sec_type == RW_STRING and in_texture:
            raw = data[body:body + sec_size].split(b"\x00", 1)[0]
            name = raw.decode("cp1251", "replace").strip()
            if name:
                out_names.append(name)
                in_texture = False  # first string = texture name, second = mask
        elif sec_type in RW_CONTAINERS:
            walk_rw(data, body, body + sec_size,
                    sec_type == RW_TEXTURE, out_names)
        pos = body + sec_size


def dff_texture_names(data):
    names = []
    walk_rw(data, 0, len(data), False, names)
    seen, ordered = set(), []
    for n in names:
        if n.lower() not in seen:
            seen.add(n.lower())
            ordered.append(n)
    return ordered


def sanitize_name(raw):
    """ASCII replacement of the same byte length for a non-ASCII texture
    name: the engine hashes and fopens the raw name bytes, and non-ASCII
    bytes do not survive the adb-push/UTF-8/fopen round trip."""
    tag = f"{zlib.crc32(raw) & 0xFFFFFFFF:08x}"
    out = ("t" + tag)[:len(raw)]
    while len(out) < len(raw):
        out += "0"
    return out.encode("ascii")


def png_size(path):
    with open(path, "rb") as f:
        header = f.read(26)
    if header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise SystemExit(f"not a PNG: {path}")
    w, h = struct.unpack(">II", header[16:24])
    return w, h


def png_has_alpha(path):
    with open(path, "rb") as f:
        header = f.read(26)
    color_type = header[25]
    return color_type in (4, 6)


def build_img(named_blobs, out_path):
    # Directory: 8-byte header + 32 bytes per entry, then sector-aligned data.
    entries = []
    dir_bytes = 8 + 32 * len(named_blobs)
    data_start = (dir_bytes + SECTOR - 1) // SECTOR
    cursor = data_start
    for name, blob in named_blobs:
        size_sectors = (len(blob) + SECTOR - 1) // SECTOR
        encoded = name.lower().encode("ascii")
        if len(encoded) > 23:
            raise SystemExit(f"entry name too long: {name}")
        entries.append((cursor, size_sectors, encoded))
        cursor += size_sectors
    with open(out_path, "wb") as f:
        f.write(b"VER2")
        f.write(struct.pack("<I", len(entries)))
        for posn, size_sectors, encoded in entries:
            f.write(struct.pack("<IHH", posn, size_sectors, 0))
            f.write(encoded.ljust(24, b"\x00"))
        f.write(b"\x00" * (data_start * SECTOR - f.tell()))
        for (posn, size_sectors, _), (_, blob) in zip(entries, named_blobs):
            f.write(blob.ljust(size_sectors * SECTOR, b"\x00"))
    return entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack", help="pack root (searched recursively)")
    ap.add_argument("--out", default="build/hdweapons-payload")
    args = ap.parse_args()

    pack = Path(args.pack)
    out = Path(args.out)
    dffs = sorted(pack.rglob("*.dff"), key=lambda p: p.name.lower())
    pngs = {}
    for p in pack.rglob("*.png"):
        pngs.setdefault(p.stem.lower(), p)
    if not dffs:
        raise SystemExit("no .dff files found under the pack root")

    (out / "hdweapons").mkdir(parents=True, exist_ok=True)
    src_dir = out / "texdb" / "hdweapons" / "src"
    src_dir.mkdir(parents=True, exist_ok=True)

    named_blobs = []
    tex_entries = {}
    missing = []
    for dff in dffs:
        blob = dff.read_bytes()
        for name in dff_texture_names(blob):
            final = name
            try:
                name.encode("ascii")
            except UnicodeEncodeError:
                raw = name.encode("cp1251", "replace")
                fixed = sanitize_name(raw)
                blob = blob.replace(raw + b"\x00", fixed + b"\x00")
                final = fixed.decode("ascii")
                print(f"renamed non-ascii texture in {dff.name}: -> {final}")
            if final in tex_entries:
                continue
            png = pngs.get(name.lower())
            if png is None:
                missing.append((dff.name, final))
                continue
            tex_entries[final] = png
        named_blobs.append((dff.name, blob))

    build_img(named_blobs, out / "hdweapons" / "hdweapons.img")
    print(f"img: {len(named_blobs)} models -> {out / 'hdweapons' / 'hdweapons.img'}")

    lines = ["cat=0 name=Default onfoot=5 slow=5 fast=5 "
             "defaultformat=0 defaultstream=0"]
    for name in sorted(tex_entries, key=str.lower):
        png = tex_entries[name]
        w, h = png_size(png)
        alpha = " alphamode=2" if png_has_alpha(png) else ""
        lines.append(f'"{name}" width={w} height={h} streammode=1{alpha}')
        target = src_dir / f"{name}.png"
        target.write_bytes(png.read_bytes())
    (out / "texdb" / "hdweapons" / "hdweapons.txt").write_text(
        "\r\n".join(lines) + "\r\n", encoding="ascii")
    print(f"texdb: {len(tex_entries)} textures -> {src_dir}")

    if missing:
        print(f"NOTE: {len(missing)} referenced textures have no PNG in the "
              "pack (they resolve from the stock gta3 database by name):")
        for dff_name, tex in missing:
            print(f"  {dff_name}: {tex}")


if __name__ == "__main__":
    main()
