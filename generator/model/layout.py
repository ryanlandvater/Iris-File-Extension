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
    # f16 has no C++20 type. It widens to float on load and narrows on store
    # (IFE_Bytes.hpp: load_f16 / store_f16); the wire width stays 2 bytes,
    # which _TYPE_WIDTH above records separately.
    "f16": "float",
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
    primitive: str  # the primitive this block derives from
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
class PrimitiveLayout:
    """One primitive block type: its inherited chain flattened into fields.

    ``fields`` is the complete prefix a derived block inherits, root-first,
    with offsets already accumulated across the chain; ``size`` is its total
    width and therefore the offset at which a block's own fields begin.
    """

    name: str
    extends: str | None
    description: str
    fields: tuple[FieldLayout, ...]
    size: int


@dataclass(frozen=True)
class LayoutResult:
    primitives: dict[str, PrimitiveLayout]
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
    """Derive every layout from the two spec documents.

    A block inherits the flattened field prefix of the primitive it names,
    then contributes its own fields at the cumulative offset that prefix
    ends at. Nothing about the prefix is assumed here: a ``file_header``
    carries no VALIDATION and a ``byte_array`` carries no STRIDE purely
    because their primitives do not declare those fields.
    """
    types = fields_doc.get("types", {})
    constants = ConstantsIndex.build(constants_doc)

    primitive_specs = {
        name: spec
        for name, spec in fields_doc.get("primitives", {}).items()
        if not is_banner(name)
    }
    primitives: dict[str, PrimitiveLayout] = {}

    def resolve(name: str, seen: tuple[str, ...] = ()) -> PrimitiveLayout:
        if name in primitives:
            return primitives[name]
        if name in seen:
            raise SpecError(f"primitive inheritance cycle involving {name!r}")
        spec = primitive_specs.get(name)
        if spec is None:
            raise SpecError(f"unknown primitive {name!r}")

        parent = spec.get("extends")
        inherited: tuple[FieldLayout, ...] = ()
        start = 0
        if parent is not None:
            base = resolve(parent, (*seen, name))
            inherited, start = base.fields, base.size

        own, end = _derive_fields(
            _concat_versioned(spec.get("fields", {}).get("ife_version", {})),
            types, constants, start,
        )
        primitives[name] = PrimitiveLayout(
            name=name,
            extends=parent,
            description=spec.get("description", ""),
            fields=(*inherited, *own),
            size=end,
        )
        return primitives[name]

    for name in primitive_specs:
        resolve(name)

    blocks: dict[str, BlockLayout] = {}
    for name, spec in fields_doc.get("blocks", {}).items():
        if is_banner(name):
            continue
        primitive_name = spec.get("primitive")
        if primitive_name is None:
            raise SpecError(f"block {name!r} names no primitive")
        primitive = primitives.get(primitive_name)
        if primitive is None:
            raise SpecError(f"block {name!r} references unknown primitive {primitive_name!r}")

        recovery = spec.get("recovery_tag")
        if recovery not in constants.recovery_values:
            raise SpecError(f"block {name!r} references unknown recovery tag {recovery!r}")

        header_fields = list(primitive.fields)
        header_size = primitive.size
        own = spec.get("fields")
        if own:
            more, header_size = _derive_fields(
                _concat_versioned(own["ife_version"]), types, constants, primitive.size,
            )
            header_fields += more

        entry_name = entry_size = None
        entry_fields: tuple[FieldLayout, ...] = ()
        entry = spec.get("entry")
        if entry is not None:
            entry_name = entry.get("name") or f"{name}_ENTRY"
            fields, end = _derive_fields(
                _concat_versioned(entry["fields"]["ife_version"]), types, constants, 0
            )
            entry_size, entry_fields = end, tuple(fields)

        blocks[name] = BlockLayout(
            name=name,
            primitive=primitive_name,
            description=spec.get("description", ""),
            recovery_tag=recovery,
            recovery_value=constants.recovery_values[recovery],
            header_fields=tuple(header_fields),
            header_size=header_size,
            from_sof=header_size if spec.get("fixed_location") else None,
            entry_name=entry_name,
            entry_size=entry_size,
            entry_fields=entry_fields,
        )

    return LayoutResult(primitives=primitives, blocks=blocks)
