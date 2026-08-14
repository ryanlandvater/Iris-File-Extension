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
# ---------------------------------------------------------------------------
# ROLE: computation only. This module turns the spec JSON into numbers —
# offsets, widths, per-version sizes — and returns them as immutable structs.
# It produces no text and performs no I/O. If you are looking for anything
# that emits C++, it is in emit/; if you are looking for file handling, it is
# in pipeline.py.
#
# Invariants:
#   * Nothing here mutates its inputs. derive_layout() takes the parsed JSON
#     and returns a fresh LayoutResult; call it twice, get two equal values.
#   * Returned mappings are in JSON document order, and that order is the byte
#     order on the wire. Iterating one is meaningful, not arbitrary.
#   * Layout structures are immutable once derived, so nothing downstream can
#     edit an offset after the fact.
# ---------------------------------------------------------------------------

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


# Recovery tags are the 0x55 high byte plus a positional sequence. The prefix
# is what makes a false positive unlikely during a corruption scan: a u16 that
# happens to equal its own offset AND begins 0x55 is rare. Named here because
# both the emitter and the validator depend on it.
RECOVERY_PREFIX = 0x5500


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


def constants_groups(doc: dict[str, Any]) -> dict[str, Any]:
    """Every constants group in the document, banners and metadata removed."""
    return {
        key: value
        for key, value in doc.items()
        if key not in ("$schema", "spec") and not is_banner(key)
    }


def is_enum_group(group: dict[str, Any]) -> bool:
    """True where a constants group is an enumeration.

    A group that declares an ``underlying_type`` enumerates the whole domain of
    some field: naming a value outside it is an error, which is what makes it
    an enum. A group without one names *points* in a domain the field defines
    for itself — the sentinels, and the orientation constants, whose field is a
    continuous f16 where any rotation is legal.

    Split on the declaration rather than on a group's name so a specification
    can hold more than one table of named values. It used to be the single
    group literally called ``statically_defined_values``, which forced the
    orientations to share a table with MAGIC_BYTES and the NULL sentinels.
    """
    return "underlying_type" in group


def value_groups(doc: dict[str, Any]) -> dict[str, Any]:
    """The non-enumeration constants groups, in document order."""
    return {
        name: group
        for name, group in constants_groups(doc).items()
        if not is_enum_group(group)
    }


def value_members(doc: dict[str, Any]) -> dict[str, Any]:
    """Every named value from every value group, flattened, oldest version first."""
    out: dict[str, Any] = {}
    for group in value_groups(doc).values():
        for _, entries in sorted(
            group.get("ife_version", {}).items(), key=lambda kv: version_key(kv[0])
        ):
            out.update(entries)
    return out


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
    since: str = "1.0"  # version group that introduced this field
    # Normative clause, from the capped predicate vocabulary: a level and
    # a spec section for the diagnostic, plus at most a minimum/maximum, an
    # ordering, an enum-membership or a non-null requirement. Carried through
    # untouched -- the model derives byte positions and takes no view on what a
    # value ought to be.
    conformance: dict[str, Any] | None = None


@dataclass(frozen=True)
class BlockLayout:
    """Derived layout of one block (universal header + array header +
    header_fields + entry)."""

    name: str
    primitive: str  # the primitive this block derives from
    description: str
    backward: bool  # fields sit before the handle's anchor, not after
    recovery_tag: str | None  # JSON name, e.g. RECOVER_FILE_HEADER; None = untagged
    recovery_value: int | None
    header_fields: tuple[FieldLayout, ...]
    header_size: int
    from_sof: int | None  # fixed_location blocks: bytes occupied from SOF
    entry_name: str | None = None
    entry_size: int | None = None
    entry_fields: tuple[FieldLayout, ...] = ()
    # Cumulative byte size at the end of each version group, ascending: the
    # data behind the emitted header_size_v1_0 / entry_size_v1_0 constants and
    # the boundary markers beside them. A reader of version N uses the size
    # recorded for N; anything past it was appended by a later version and is
    # gated off. With today's spec both are exactly (("1.0", <total>),).
    header_sizes: tuple[tuple[str, int], ...] = ()
    entry_sizes: tuple[tuple[str, int], ...] = ()
    # Written from key/value pairs: the writer derives both the sizes array
    # and the packed byte run from a single `vector<pair<string,string>>`,
    # so the slicing cannot drift from the bytes it describes. Declared in
    # the schema ("from_pairs": true) -- a writer-convenience marker, not a
    # statement about the wire layout.
    from_pairs: bool = False


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
    # True where the primitive lays its fields out before the handle's anchor
    # rather than after it. Derived blocks inherit the direction.
    backward: bool = False


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
        # Every group, enumeration or not: a field that names a value group in
        # its `enum` key must fail with "missing underlying_type" rather than
        # with "unknown group", which is the more useful of the two errors.
        groups = constants_groups(doc)
        sentinels: dict[str, dict[str, Any]] = value_members(doc)
        # Prefix plus the authored sequence number, matching the emitter.
        recovery_values: dict[str, int] = {}
        for version_entries in doc.get("recovery_codes", {}).get("ife_version", {}).values():
            for name, raw in version_entries.items():
                sequence = raw["value"] if isinstance(raw, dict) else raw
                recovery_values[name] = RECOVERY_PREFIX + parse_int(sequence)
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


# Returns pairs, not a flat list: the version label travels with each field so
# _derive_fields can stamp it as `since`.
def _concat_versioned(groups: dict[str, list]) -> list[tuple[str, dict[str, Any]]]:
    """Flatten versioned field groups into (version, field) pairs, ascending.

    The version label is carried rather than dropped: it becomes each field's
    ``since``, which is the only thing that lets an emitter tell a 1.0 field
    from one appended later. Ordering is by version, then document order
    within a version — the concatenation that defines the layout.
    """
    ordered = sorted(groups.items(), key=lambda kv: version_key(kv[0]))
    return [(version, field) for version, fields in ordered for field in fields]


def _derive_fields(
    field_specs: list[tuple[str, dict[str, Any]]],
    types: dict[str, Any],
    constants: ConstantsIndex,
    start_offset: int,
    backward: bool = False,
) -> tuple[list[FieldLayout], int, list[tuple[str, int]]]:
    """Compute offsets for one field list.

    Returns (fields, end_offset, sizes_by_version) where the third element is
    the cumulative end offset at the close of each version group, ascending —
    what a version-gated reader needs to know how far its own version extends.

    ``backward`` lays the fields out *before* the anchor a handle is built at,
    the first declared field ending at the anchor and each one after it sitting
    further back. Offsets come out negative and the cumulative width stays
    positive, so `end_offset` is a size in both directions.

    This is what makes a trailer extensible. Growing a forward block moves its
    end; growing a backward one moves its *start*, leaving every field already
    published at the displacement it has always had — which matters when the
    only way to find the block is to measure back from something else.
    """
    fields: list[FieldLayout] = []
    seen: set[str] = set()
    offset = start_offset
    sizes: list[tuple[str, int]] = []
    current: str | None = None

    for version, spec in field_specs:
        if current is not None and version != current:
            sizes.append((current, offset))
        current = version

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
            kind, type_name = "enum", spec["enum"]
        else:
            type_name = spec["type"]
            size, cpp = _width_cpp(type_name, types)
            kind = "constant" if "constant" in spec else "scalar"

        fields.append(
            FieldLayout(
                name=name,
                type_name=type_name,
                size=size,
                offset=-(offset + size) if backward else offset,
                cpp_type=cpp,
                kind=kind,
                description=spec.get("description", ""),
                constant=spec.get("constant"),
                points_to=spec.get("points_to"),
                nullable=spec.get("nullable"),
                since=version,
                conformance=spec.get("conformance"),
            )
        )
        offset += size

    if current is not None:
        sizes.append((current, offset))
    return fields, offset, sizes


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

    # `seen` carries the ancestry down the recursion so an inheritance cycle
    # is caught rather than recursed into; callers start with an empty chain.
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

        # Primitives take no sizes-by-version, and that is not an oversight: a
        # primitive can never gain a field. Appending to a prefix would shift
        # the own-fields of every block deriving from it — moving fields that
        # already shipped, which the append-only invariant forbids. Once a
        # block derives from a primitive, that primitive is frozen; new data
        # is appended to the block or its entry instead.
        backward = spec.get("layout") == "trailer" or (
            parent is not None and primitives[parent].backward
        )
        own, end, _ = _derive_fields(
            _concat_versioned(spec.get("fields", {}).get("ife_version", {})),
            types, constants, start, backward,
        )
        primitives[name] = PrimitiveLayout(
            name=name,
            extends=parent,
            description=spec.get("description", ""),
            fields=(*inherited, *own),
            size=end,
            backward=backward,
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

        # A null tag is a block that the recovery scan does not find by tag;
        # see the validator for why that is allowed and which block does it.
        recovery = spec.get("recovery_tag")
        if recovery is not None and recovery not in constants.recovery_values:
            raise SpecError(f"block {name!r} references unknown recovery tag {recovery!r}")

        header_fields = list(primitive.fields)
        header_size = primitive.size
        header_sizes: list[tuple[str, int]] = []
        own = spec.get("fields")
        if own:
            more, header_size, header_sizes = _derive_fields(
                _concat_versioned(own["ife_version"]), types, constants, primitive.size,
                primitive.backward,
            )
            header_fields += more
        if not header_sizes:
            # No own fields: the block is exactly its primitive, at 1.0.
            header_sizes = [("1.0", header_size)]

        entry_name = entry_size = None
        entry_fields: tuple[FieldLayout, ...] = ()
        entry_sizes: list[tuple[str, int]] = []
        entry = spec.get("entry")
        if entry is not None:
            entry_name = entry.get("name") or f"{name}_ENTRY"
            fields, end, entry_sizes = _derive_fields(
                _concat_versioned(entry["fields"]["ife_version"]), types, constants, 0
            )
            entry_size, entry_fields = end, tuple(fields)

        blocks[name] = BlockLayout(
            name=name,
            primitive=primitive_name,
            description=spec.get("description", ""),
            backward=primitive.backward,
            recovery_tag=recovery,
            recovery_value=constants.recovery_values[recovery] if recovery else None,
            header_fields=tuple(header_fields),
            header_size=header_size,
            from_sof=header_size if spec.get("fixed_location") else None,
            entry_name=entry_name,
            entry_size=entry_size,
            entry_fields=entry_fields,
            header_sizes=tuple(header_sizes),
            entry_sizes=tuple(entry_sizes),
            from_pairs=spec.get("from_pairs", False),
        )

    return LayoutResult(primitives=primitives, blocks=blocks)
