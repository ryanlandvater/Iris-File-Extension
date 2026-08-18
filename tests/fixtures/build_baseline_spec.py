#!/usr/bin/env python3
"""Build a 1.0-only spec, so the corpus can contain a genuine 1.0 file.

The generated `store()` always lays out the newest version it knows — that is
deliberate, and it is why a writer built from the committed spec can only ever
produce a 1.1 file. But the oracle's substance is a *1.1 reader over 1.0
bytes*: that TILE_LENGTH and Z_PLANES read back absent, that the layer extent
stride is the 1.0 entry size, that a newer build reads an older file to the
extent of the fields it knows. A 1.1 fixture cannot prove any of it, and the
old v1-written 1.0 fixture stopped being loadable when the ATTRIBUTE_SIZE
correction moved the stride floor from six to seven.

So the fixture writer is built against a *derived* 1.0 schema rather than the
committed one: every "1.1" field group is dropped and the document version is
set to 1.0. The bytes it writes are a conformant 1.0 file — including the
corrected seven-byte ATTRIBUTE_SIZE entry, which is what 1.0 has always
specified since the correction.

Derived rather than kept as a second copy, for the reason build_future_spec.py
gives: a hand-maintained parallel spec drifts, and a fixture that drifts from
the specification is evidence of nothing.

Usage: build_baseline_spec.py <spec_dir> <out_dir>
"""
import json
import sys
from pathlib import Path

BASELINE = "1.0"


def strip_newer(groups: dict) -> dict:
    """Keep only the baseline version's field group."""
    return {v: fields for v, fields in groups.items() if v == BASELINE}


def strip_block(block: dict) -> None:
    """Drop post-baseline groups from a block and, if present, its entry."""
    for owner in (block, block.get("entry")):
        if not isinstance(owner, dict):
            continue
        groups = owner.get("fields", {}).get("ife_version")
        if isinstance(groups, dict):
            owner["fields"]["ife_version"] = strip_newer(groups)


def main() -> int:
    spec_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    fields = json.loads((spec_dir / "ife_fields.json").read_text())
    constants = json.loads((spec_dir / "ife_constants.json").read_text())
    header = json.loads((spec_dir / "ife_header.json").read_text())

    # A block whose every field group postdates the baseline did not exist at
    # 1.0 and is dropped whole, along with any offset field pointing at it —
    # a dangling points_to is a spec error, and rightly so.
    dropped = set()
    for name, block in fields["blocks"].items():
        if name.startswith("//"):
            continue
        groups = block.get("fields", {}).get("ife_version", {})
        if groups and not any(v == BASELINE for v in groups):
            dropped.add(name)
    for name in dropped:
        del fields["blocks"][name]

    for name, block in fields["blocks"].items():
        if name.startswith("//"):
            continue
        strip_block(block)

    # Drop any field still pointing at a block that is gone. Today none
    # survives -- the fields naming the 1.1 blocks are themselves 1.1 and were
    # stripped above -- but that is a property of this schema, not of the
    # derivation, and a dangling points_to fails --validate at configure time
    # with an error pointing at a file nobody edited.
    for name, block in fields["blocks"].items():
        if name.startswith("//"):
            continue
        for owner in (block, block.get("entry")):
            if not isinstance(owner, dict):
                continue
            groups = owner.get("fields", {}).get("ife_version", {})
            for version, group in groups.items():
                groups[version] = [f for f in group
                                   if f.get("points_to") not in dropped]
    for primitive in fields["primitives"].values():
        strip_block(primitive)
    # A primitive left with no fields at all never existed at the baseline.
    fields["primitives"] = {
        n: p for n, p in fields["primitives"].items()
        if p.get("fields", {}).get("ife_version")
    }

    for group in constants.values():
        if isinstance(group, dict) and isinstance(group.get("ife_version"), dict):
            group["ife_version"] = strip_newer(group["ife_version"])

    header["version"] = {"major": 1, "minor": 0}
    header["revisions"] = [r for r in header.get("revisions", [])
                           if r.get("version") == BASELINE]

    for name, doc in (("ife_fields.json", fields),
                      ("ife_constants.json", constants),
                      ("ife_header.json", header)):
        (out_dir / name).write_text(json.dumps(doc, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
