#!/usr/bin/env python3
"""Build the synthetic future-version fixture spec used by ife_version_gating_tests.

The committed spec is 1.0-only, so the since-mechanism in the generated
layer — optional accessors gated by the file version and the stored stride —
has never executed. Rather than maintain a second copy of the spec that can
silently drift from the real one, this derives the fixture: the committed
JSON plus exactly two injected field groups and a version bump.

    FILE_HEADER.fields        + RUNTIME_FLAGS   (u32, block-level gating)
    LAYER_EXTENTS entry       + RESERVED_EXTENT (u16, stride-gated)

The version is deliberately extreme (200.0): the test then proves
the version comparison is numeric, not lexicographic.

Usage: build_future_spec.py <spec_dir> <out_dir>
"""
import json
import sys
from pathlib import Path

# The repo root, so the fixture's derived sizes come from the same layout
# model the generator uses — the tests then reference one derivation, not
# literals that can drift from it.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from generator.model.layout import derive_layout  # noqa: E402

INJECTED_FIELDS = [
    {
        "name": "RUNTIME_FLAGS",
        "type": "u32",
        "description": "Synthetic 200.0 fixture field: runtime flags.",
    },
    {
        "name": "RESERVED_EXTENT",
        "type": "u16",
        "description": "Synthetic 200.0 fixture field: per-extent reservation.",
    },
]


FIXTURE_VERSION = {"major": 200, "minor": 0}


def main() -> int:
    spec_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    fields = json.loads((spec_dir / "ife_fields.json").read_text())
    constants = json.loads((spec_dir / "ife_constants.json").read_text())
    fields["blocks"]["FILE_HEADER"]["fields"]["ife_version"]["200.0"] = [INJECTED_FIELDS[0]]
    fields["blocks"]["LAYER_EXTENTS"]["entry"]["fields"]["ife_version"]["200.0"] = [INJECTED_FIELDS[1]]

    header = json.loads((spec_dir / "ife_header.json").read_text())
    header["version"] = FIXTURE_VERSION

    documents = [
        ("ife_fields.json", fields),
        ("ife_constants.json", constants),
        ("ife_header.json", header),
    ]
    for name, doc in documents:
        (out_dir / name).write_text(json.dumps(doc, indent=2) + "\n")

    # The derived layout as a C++ header: sizes and offsets of the injected
    # fields, computed by the same derive_layout the generator runs. The
    # version-gating tests include it instead of hardcoding byte counts, so
    # they keep working as the real spec grows.
    layout = derive_layout(fields, constants)
    fh = layout.blocks["FILE_HEADER"]
    le = layout.blocks["LAYER_EXTENTS"]
    flags = next(f for f in fh.header_fields if f.name == "RUNTIME_FLAGS")
    reserved = next(f for f in le.entry_fields if f.name == "RESERVED_EXTENT")
    # The prefix boundary: the real spec's newest version's size, where the
    # injected 200.0 fields begin. The injected group is always last, so the
    # second-to-last entry is the real spec's newest table.
    fh_prefix = fh.header_sizes[-2][1] if len(fh.header_sizes) > 1 else fh.header_sizes[0][1]
    le_prefix = le.entry_sizes[-2][1] if len(le.entry_sizes) > 1 else le.entry_sizes[0][1]
    (out_dir / "ife_fixture_layout.hpp").write_text(
        "// Derived by build_future_spec.py through generator.model.layout.\n"
        "// Do not hand-edit; re-run CMake configure to regenerate.\n"
        "#pragma once\n"
        "#include <cstdint>\n"
        "namespace ife_test {\n"
        f"constexpr std::uint64_t FILE_HEADER_HEADER_SIZE = {fh.header_size};\n"
        f"constexpr std::uint64_t LAYER_EXTENT_ENTRY_SIZE = {le.entry_size};\n"
        f"constexpr std::uint64_t FILE_HEADER_PREFIX_SIZE = {fh_prefix};\n"
        f"constexpr std::uint64_t LAYER_EXTENT_PREFIX_SIZE = {le_prefix};\n"
        f"constexpr std::uint64_t RUNTIME_FLAGS_AT        = {flags.offset};\n"
        f"constexpr std::uint64_t RESERVED_EXTENT_AT      = {reserved.offset};\n"
        "}\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
