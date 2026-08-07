"""Layout derivation for the IFE spec JSON.

Offsets and sizes are never stated in the spec (spec/README.md): layouts
are derived by concatenating version groups in ascending order and
accumulating field widths. This module is the single implementation of
that rule, shared by the C++ and doc emitters so the two outputs cannot
drift.

Derivation rules:
  * every block starts with the universal block header (10 B);
  * array blocks add the uniform array header (STRIDE u16 + COUNT u32);
  * block-specific ``header_fields`` (arrays) follow the array header;
  * array entries have their own versioned field groups, or are blobs
    (stride 1);
  * FILE_HEADER is fixed at preamble + header size from SOF.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any

# Canonical type names -> on-wire width and natural C++ type. Enum-typed
# fields resolve through the constants doc's underlying_type instead.
_TYPE_WIDTH: dict[str, int] = {
    "u8": 1, 
    "u16": 2, 
    "u24": 3, 
    "u32": 4, 
    "u40": 5, 
    "u64": 8,
    "f16": 2, 
    "f32": 4, 
    "f64": 8,
}
_TYPE_CPP: dict[str, str] = {
    "u8": "std::uint8_t",
    "u16": "std::uint16_t",
    "u24": "std::uint32_t",
    "u32": "std::uint32_t",
    "u40": "std::uint64_t",
    "u64": "std::uint64_t",
    "f16": "std::uint16_t",
    "f32": "float",
    "f64": "double",
}


class SpecError(ValueError):
    """The spec JSON violates a derivation invariant; message names the value."""


def is_banner(key: str) -> bool:
    """True for a documentation banner key ("// MARK: - ...").

    JSON has no comment syntax, so the spec documents mark their section
    boundaries with keys every consumer skips — the same ``// MARK:``
    convention the hand-written headers use. Defined once here so the
    layout model and both emitters cannot disagree about what a banner is.
    """
    return key.startswith("//")


def version_key(version: str) -> tuple[int, int]:
    """Sort key for '1.0' / '1.1' version group labels."""
    major, _, minor = version.partition(".")
    return int(major), int(minor or 0)


def parse_int(value: Any) -> int:
    """Parse a constant value that may be an int, decimal string, or 0x hex."""
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        return int(value, 0)
    raise SpecError(f"non-integer constant value {value!r}")


@dataclass(frozen=True)
class FieldLayout:
    """One derived field: offset/size computed, never read from the JSON."""

    name: str
    type_name: str  # canonical scalar name (e.g. "u32") or enum group (e.g. "tile_encodings")
    size: int
    offset: int
    cpp_type: str
    kind: str  # "scalar" | "enum" | "constant"
    description: str = ""
    constant: str | None = None
    points_to: str | None = None
    nullable: bool | None = None


@dataclass(frozen=True)
class BlockLayout:
    """Derived layout of one block (universal header + array header +
    header_fields + entry)."""

    name: str
    kind: str  # "header" | "array"
    description: str
    recovery_tag: str  # JSON name, e.g. RECOVER_FILE_HEADER
    recovery_value: int
    header_fields: tuple[FieldLayout, ...]
    header_size: int
    from_sof: int | None  # fixed_location blocks: preamble + header_size
    entry_name: str | None = None
    entry_size: int | None = None  # 1 for byte blobs
    entry_fields: tuple[FieldLayout, ...] = ()


@dataclass(frozen=True)
class LayoutResult:
    preamble_fields: tuple[FieldLayout, ...]
    preamble_size: int
    block_header_fields: tuple[FieldLayout, ...]
    block_header_size: int
    array_header_fields: tuple[FieldLayout, ...]
    array_header_size: int
    blocks: dict[str, BlockLayout]


@dataclass
class ConstantsIndex:
    """Indexed constants doc: enum groups, sentinels, recovery values."""

    groups: dict[str, dict[str, Any]]
    sentinels: dict[str, dict[str, Any]]
    recovery_values: dict[str, int]
    recovery_enum_group: str = "recovery_codes"

    @classmethod
    def build(cls, doc: dict[str, Any]) -> "ConstantsIndex":
        groups = {
            key: value
            for key, value in doc.items()
            if key not in ("$schema", "spec", "statically_defined_values")
            and not is_banner(key)
        }
        sentinels: dict[str, dict[str, Any]] = {}
        for version_entries in doc.get("statically_defined_values", {}).get("ife_version", {}).values():
            sentinels.update(version_entries)
        recovery_values: dict[str, int] = {}
        for version_entries in doc.get("recovery_codes", {}).get("ife_version", {}).values():
            for name, raw in version_entries.items():
                recovery_values[name] = parse_int(raw["value"] if isinstance(raw, dict) else raw)
        return cls(groups=groups, sentinels=sentinels, recovery_values=recovery_values)


def _canonical_type(name: str, types: dict[str, Any]) -> str:
    """Resolve alias chains (offset -> u64, recovery -> u16, byte -> u8)."""
    seen: set[str] = set()
    while True:
        if name not in types:
            raise SpecError(f"unknown field type {name!r}")
        if name in seen:
            raise SpecError(f"type alias cycle involving {name!r}")
        seen.add(name)
        entry = types[name]
        if "alias" not in entry:
            return name
        name = entry["alias"]


def _width_cpp(type_name: str, types: dict[str, Any]) -> tuple[int, str]:
    canonical = _canonical_type(type_name, types)
    try:
        return _TYPE_WIDTH[canonical], _TYPE_CPP[canonical]
    except KeyError:
        raise SpecError(f"type {type_name!r} resolves to unsupported canonical {canonical!r}") from None


def _concat_versioned(groups: dict[str, list]) -> list[dict[str, Any]]:
    """Concatenate versioned field groups in ascending version order."""
    ordered = sorted(groups.items(), key=lambda kv: version_key(kv[0]))
    return [field for _, fields in ordered for field in fields]


def _derive_fields(
    field_specs: list[dict[str, Any]],
    types: dict[str, Any],
    constants: ConstantsIndex,
    start_offset: int,
) -> tuple[list[FieldLayout], int]:
    """Compute offsets for one field list; returns (fields, end_offset)."""
    fields: list[FieldLayout] = []
    seen: set[str] = set()
    offset = start_offset
    for spec in field_specs:
        name = spec.get("name")
        if not name:
            raise SpecError(f"field missing 'name' in {field_specs!r}")
        if name in seen:
            raise SpecError(f"duplicate field {name!r}")
        seen.add(name)

        if "enum" in spec:
            group = constants.groups.get(spec["enum"])
            if group is None:
                raise SpecError(f"field {name!r} references unknown enum group {spec['enum']!r}")
            underlying = group.get("underlying_type")
            if underlying is None:
                raise SpecError(f"enum group {spec['enum']!r} missing underlying_type")
            size, cpp = _width_cpp(underlying, types)
            kind, type_name, enum_group = "enum", spec["enum"], spec["enum"]
        else:
            type_name = spec["type"]
            size, cpp = _width_cpp(type_name, types)
            kind = "constant" if "constant" in spec else "scalar"
            enum_group = None

        fields.append(
            FieldLayout(
                name=name,
                type_name=type_name,
                size=size,
                offset=offset,
                cpp_type=cpp,
                kind=kind,
                description=spec.get("description", ""),
                constant=spec.get("constant"),
                points_to=spec.get("points_to"),
                nullable=spec.get("nullable"),
            )
        )
        offset += size
    return fields, offset


def derive_layout(fields_doc: dict[str, Any], constants_doc: dict[str, Any]) -> LayoutResult:
    """Derive every layout from the two spec documents."""
    types = fields_doc.get("types", {})
    constants = ConstantsIndex.build(constants_doc)

    def structure(key: str) -> tuple[tuple[FieldLayout, ...], int]:
        groups = fields_doc[key]["fields"]["ife_version"]
        fields, end = _derive_fields(_concat_versioned(groups), types, constants, 0)
        return tuple(fields), end

    preamble_fields, preamble_size = structure("file_preamble")
    block_header_fields, block_header_size = structure("block_header")

    # Array header = universal block header + STRIDE + COUNT (contiguous).
    array_specific, _ = _derive_fields(
        _concat_versioned(fields_doc["array_header"]["fields"]["ife_version"]),
        types, constants, block_header_size,
    )
    array_header_fields = (*block_header_fields, *array_specific)
    array_header_size = array_header_fields[-1].offset + array_header_fields[-1].size

    blocks: dict[str, BlockLayout] = {}
    for name, spec in fields_doc.get("blocks", {}).items():
        if is_banner(name):
            continue
        kind = spec.get("kind")
        if kind not in ("header", "array"):
            raise SpecError(f"block {name!r} has invalid kind {kind!r}")
        recovery = spec.get("recovery_tag")
        if recovery not in constants.recovery_values:
            raise SpecError(f"block {name!r} references unknown recovery tag {recovery!r}")

        header_fields = list(block_header_fields)
        if kind == "array":
            header_fields += array_specific
            block_specific = spec.get("header_fields")
        else:
            block_specific = spec.get("fields")
        if block_specific:
            # Block-specific fields follow the headers contiguously: the
            # start offset is the cumulative byte size, not the field count.
            start = header_fields[-1].offset + header_fields[-1].size
            more, _ = _derive_fields(
                _concat_versioned(block_specific["ife_version"]),
                types, constants, start,
            )
            header_fields += more
        header_size = header_fields[-1].offset + header_fields[-1].size

        entry_name = entry_size = None
        entry_fields: tuple[FieldLayout, ...] = ()
        if kind == "array":
            entry = spec.get("entry", {})
            if entry.get("blob"):
                entry_size = 1
            else:
                entry_name = entry.get("name") or f"{name}_ENTRY"
                fields, end = _derive_fields(
                    _concat_versioned(entry["fields"]["ife_version"]), types, constants, 0
                )
                entry_name, entry_size, entry_fields = entry_name, end, tuple(fields)

        blocks[name] = BlockLayout(
            name=name,
            kind=kind,
            description=spec.get("description", ""),
            recovery_tag=recovery,
            recovery_value=constants.recovery_values[recovery],
            header_fields=tuple(header_fields),
            header_size=header_size,
            from_sof=preamble_size + header_size if spec.get("fixed_location") else None,
            entry_name=entry_name,
            entry_size=entry_size,
            entry_fields=entry_fields,
        )

    return LayoutResult(
        preamble_fields=preamble_fields,
        preamble_size=preamble_size,
        block_header_fields=block_header_fields,
        block_header_size=block_header_size,
        array_header_fields=array_header_fields,
        array_header_size=array_header_size,
        blocks=blocks,
    )
