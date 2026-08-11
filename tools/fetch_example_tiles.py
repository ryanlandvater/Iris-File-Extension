#!/usr/bin/env python3
"""Pull real compressed tiles out of a published Iris slide, over HTTP ranges.

The conformance fixtures (plans/phase-6-corpus.md) carry genuine encoder output
rather than synthetic bytes, but the published slides are gigabytes and 99.9%
of that is pixel data we do not want.  This walks the structure with range
requests -- header, tile table, layer extents, tile offsets -- and then fetches
only the handful of tile byte-ranges asked for.  A dozen tiles costs a few
hundred kilobytes and about four round trips.

Nothing here interprets a tile.  IFE does not decode pixels, and neither does
this: a tile is an opaque byte range identified by the offsets array.

Stdlib only, matching generator/.  Offsets come from spec/*.json via the same
layout derivation the C++ emitter uses, so this file states no byte offset of
its own.

Usage:
    python3 tools/fetch_example_tiles.py --list
    python3 tools/fetch_example_tiles.py --slide JPEG --layer 0 --count 12 \\
            --out tests/corpus/tiles
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import urllib.request

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

BASE = ("https://media.githubusercontent.com/media/IrisDigitalPathology/"
        "Iris-Example-Files/main/Cervial%20Biopsy")

SLIDES = {
    "JPEG": f"{BASE}/425248_JPEG.iris",
    "AVIF": f"{BASE}/425248_AVIF.iris",
}


# ---------------------------------------------------------------- transport --

class Ranged:
    """A read-only file that lives on a web server.

    Caches every range it fetches: the structural walk re-reads the same few
    hundred bytes repeatedly, and a cache turns that into one request.
    """

    def __init__(self, url: str) -> None:
        self.url = url
        self._cache: dict[tuple[int, int], bytes] = {}
        self.requests = 0
        self.fetched = 0

    def get(self, offset: int, size: int) -> bytes:
        key = (offset, size)
        if key in self._cache:
            return self._cache[key]
        req = urllib.request.Request(
            self.url, headers={"Range": f"bytes={offset}-{offset + size - 1}"})
        with urllib.request.urlopen(req) as r:
            if r.status != 206:
                raise RuntimeError(
                    f"{self.url} answered {r.status}, not 206 -- the host must "
                    f"support range requests for this tool to be worth using")
            data = r.read()
        if len(data) != size:
            raise RuntimeError(f"asked for {size} B at {offset}, got {len(data)}")
        self._cache[key] = data
        self.requests += 1
        self.fetched += len(data)
        return data

    def u(self, offset: int, width: int) -> int:
        return int.from_bytes(self.get(offset, width), "little")


# ------------------------------------------------------------------- layout --

def layout():
    """Field offsets, derived from the JSON rather than restated here."""
    from generator.model.layout import derive_layout

    fields_doc = json.loads((REPO / "spec/ife_fields.json").read_text())
    constants_doc = json.loads((REPO / "spec/ife_constants.json").read_text())
    return derive_layout(fields_doc, constants_doc)


def field(blocks, block: str, name: str) -> tuple[int, int]:
    """(offset, size) of a header field. header_fields already carries the
    primitive prefix -- VALIDATION, RECOVERY, STRIDE, COUNT -- so there is one
    place to look."""
    for f in blocks[block].header_fields:
        if f.name == name:
            return f.offset, f.size
    raise KeyError(f"{block}.{name} is not in the schema")


def entry_field(blocks, block: str, name: str) -> tuple[int, int]:
    """(offset, size) of a field within one array entry, relative to it."""
    for f in blocks[block].entry_fields:
        if f.name == name:
            return f.offset, f.size
    raise KeyError(f"{block}[].{name} is not in the schema")


# --------------------------------------------------------------------- walk --

def walk(src: Ranged, blocks) -> dict:
    """Follow header -> tile table -> extents + offsets, reading only headers."""
    def fv(block, name, base):
        off, size = field(blocks, block, name)
        return src.u(base + off, size)

    file_size = fv("FILE_HEADER", "FILE_SIZE", 0)
    major = fv("FILE_HEADER", "EXTENSION_MAJOR", 0)
    minor = fv("FILE_HEADER", "EXTENSION_MINOR", 0)
    table = fv("FILE_HEADER", "TILE_TABLE_OFFSET", 0)

    encoding = fv("TILE_TABLE", "ENCODING", table)
    fmt = fv("TILE_TABLE", "FORMAT", table)
    offsets_at = fv("TILE_TABLE", "TILE_OFFSETS_OFFSET", table)
    extents_at = fv("TILE_TABLE", "LAYER_EXTENTS_OFFSET", table)

    ext_stride = fv("LAYER_EXTENTS", "STRIDE", extents_at)
    ext_count = fv("LAYER_EXTENTS", "COUNT", extents_at)
    ext_header = blocks["LAYER_EXTENTS"].header_size

    x_off, x_sz = entry_field(blocks, "LAYER_EXTENTS", "X_TILES")
    y_off, y_sz = entry_field(blocks, "LAYER_EXTENTS", "Y_TILES")
    layers = []
    for i in range(ext_count):
        e = extents_at + ext_header + i * ext_stride
        layers.append({"x_tiles": src.u(e + x_off, x_sz),
                       "y_tiles": src.u(e + y_off, y_sz)})

    off_stride = fv("TILE_OFFSETS", "STRIDE", offsets_at)
    off_count = fv("TILE_OFFSETS", "COUNT", offsets_at)

    return {
        "file_size": file_size, "version": f"{major}.{minor}",
        "encoding": encoding, "format": fmt,
        "tile_offsets_at": offsets_at, "tile_stride": off_stride,
        "tile_count": off_count,
        "tile_offsets_header": blocks["TILE_OFFSETS"].header_size,
        "layers": layers,
    }


def tile_entries(src: Ranged, blocks, info: dict, first: int, count: int):
    """Read `count` tile entries starting at flat index `first`."""
    o_off, o_sz = entry_field(blocks, "TILE_OFFSETS", "OFFSET")
    s_off, s_sz = entry_field(blocks, "TILE_OFFSETS", "SIZE")
    base = info["tile_offsets_at"] + info["tile_offsets_header"]
    stride = info["tile_stride"]

    span = src.get(base + first * stride, count * stride)
    out = []
    for i in range(count):
        e = span[i * stride:(i + 1) * stride]
        out.append({
            "index": first + i,
            "offset": int.from_bytes(e[o_off:o_off + o_sz], "little"),
            "size": int.from_bytes(e[s_off:s_off + s_sz], "little"),
        })
    return out


def flat_index(info: dict, layer: int) -> int:
    """Tile entries are one flat array; layer N starts after all before it."""
    return sum(l["x_tiles"] * l["y_tiles"] for l in info["layers"][:layer])


# --------------------------------------------------------------------- main --

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--slide", choices=sorted(SLIDES), default="JPEG")
    ap.add_argument("--url", help="override the published slide URL")
    ap.add_argument("--layer", type=int, default=0)
    ap.add_argument("--count", type=int, default=12)
    # Into .deps/ rather than the source tree: these are inputs to fixture
    # generation, not artifacts, and the repository stays blob-free.
    ap.add_argument("--out", type=pathlib.Path, default=REPO / ".deps/example_tiles")
    ap.add_argument("--list", action="store_true",
                    help="describe the slide's structure and exit")
    args = ap.parse_args()

    src = Ranged(args.url or SLIDES[args.slide])
    blocks = layout().blocks
    info = walk(src, blocks)

    print(f"{src.url}")
    print(f"  IFE {info['version']}, {info['file_size']:,} B, "
          f"encoding={info['encoding']} format={info['format']}")
    print(f"  {len(info['layers'])} layers, {info['tile_count']:,} tile entries")
    for i, l in enumerate(info["layers"]):
        n = l["x_tiles"] * l["y_tiles"]
        print(f"    layer {i}: {l['x_tiles']}x{l['y_tiles']} = {n:,} tiles")
    if args.list:
        print(f"\n  {src.requests} range requests, {src.fetched:,} B fetched")
        return 0

    if not 0 <= args.layer < len(info["layers"]):
        print(f"layer {args.layer} out of range", file=sys.stderr)
        return 2

    first = flat_index(info, args.layer)
    avail = info["layers"][args.layer]["x_tiles"] * info["layers"][args.layer]["y_tiles"]
    count = min(args.count, avail)
    entries = tile_entries(src, blocks, info, first, count)

    args.out.mkdir(parents=True, exist_ok=True)
    manifest = {"source": src.url, "ife_version": info["version"],
                "encoding": info["encoding"], "format": info["format"],
                "layer": args.layer, "tiles": []}

    print(f"\n  fetching {count} tiles from layer {args.layer} "
          f"(flat index {first}..{first + count - 1})")
    total = 0
    for e in entries:
        data = src.get(e["offset"], e["size"])
        name = f"tile_{e['index']:06d}.bin"
        (args.out / name).write_bytes(data)
        manifest["tiles"].append({"file": name, "index": e["index"],
                                  "size": e["size"]})
        total += e["size"]
        print(f"    {name}  {e['size']:>8,} B  @ {e['offset']:,}")

    (args.out / "tiles.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\n  {total:,} B of tile data -> {args.out}")
    print(f"  {src.requests} range requests, {src.fetched:,} B fetched total")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
