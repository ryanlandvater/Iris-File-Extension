"""The wire witness: the facts of the spec documents that serialize to disk.

THE GATE is wire stability, not C++ source text identity. The emitted headers
may be reformatted at will; this witness ignores C++ text entirely and captures
only what a conformant encoder writes into an .iris stream:

  * blocks     — recovery tag value, primitive, fixed file position, and
                 every field's (name, type-or-enum-group, width, offset),
                 grouped by the version that introduced it. Offsets come from
                 generator.model.layout.derive_layout — the same derivation
                 the emitters and the reader use — never re-computed here.
  * entries    — the same for every array block's entry, plus the entry size
                 at the close of each version group.
  * enums      — each group's underlying_type and every member's name -> value
                 (recovery tags with the 0x55 prefix applied).
  * constants  — every named value (MAGIC_BYTES, the NULL sentinels, the
                 orientation constants, ...) as its on-wire integer.

If the witness JSON is unchanged across a refactor, the wire format is
preserved regardless of how the emitting Python was reorganised.

``witness_hash`` fingerprints the whole witness for the generated banners:
`--check` verifies a file was produced from the current spec by comparing
its banner hash against a fresh computation (XP-2).
"""

from __future__ import annotations

import hashlib
import json
from typing import Any

from .model.layout import (
    ConstantsIndex,
    FieldLayout,
    derive_layout,
    is_enum_group,
    parse_int,
    value_members,
    version_key,
)


def _fields_by_version(fields: tuple[FieldLayout, ...]) -> dict[str, list[list[Any]]]:
    """(version -> [(name, type or enum group, width, offset)]) in layout order.

    FieldLayout.since is the version group that introduced the field; grouping
    on it keeps the wire-visible growth history instead of a flat field list.
    """
    out: dict[str, list[list[Any]]] = {}
    for field in fields:
        out.setdefault(field.since, []).append(
            [field.name, field.type_name, field.size, field.offset]
        )
    return out


def witness(
    fields_doc: dict[str, Any],
    constants_doc: dict[str, Any],
    header_doc: dict[str, Any],
) -> dict[str, Any]:
    """Capture every wire-format-relevant fact of the spec documents.

    Returns only what reaches the wire, never C++ text. ``header_doc`` is
    accepted so a caller loads the three documents exactly as the pipeline
    does; it carries no wire facts of its own (the schema version is written
    by the FILE_HEADER block's EXTENSION_MAJOR/MINOR fields, which are
    witnessed under ``blocks``).
    """
    layout = derive_layout(fields_doc, constants_doc)
    index = ConstantsIndex.build(constants_doc)

    blocks: dict[str, Any] = {}
    entries: dict[str, Any] = {}
    for name, block in layout.blocks.items():
        blocks[name] = {
            # The u16 that actually lands in the block's RECOVERY bytes (0x55
            # prefix included); None where the block carries no tag.
            "recovery_tag": block.recovery_value,
            "primitive": block.primitive,
            # Whether the block is pinned to a byte position in the file
            # rather than reached through an offset field, and how much of the
            # file it claims. Recorded for every block, None included: field
            # offsets are block-relative and identical either way, so a root
            # that stopped being fixed at byte 0 -- or an ordinary block that
            # started being pinned -- would move real bytes while every other
            # fact in this witness stayed the same.
            "from_sof": block.from_sof,
            "fields": _fields_by_version(block.header_fields),
        }
        if block.entry_fields:
            entries[name] = {
                "fields": _fields_by_version(block.entry_fields),
                # entry_size at the close of each version group: a reader of
                # version N uses the size recorded for N, so every group's
                # boundary is a wire fact, not just the final total.
                "entry_size": dict(block.entry_sizes),
            }

    enums: dict[str, Any] = {}
    for group_name, group in index.groups.items():
        if not is_enum_group(group):
            continue
        members: dict[str, int] = {}
        for _, version_entries in sorted(
            group.get("ife_version", {}).items(), key=lambda kv: version_key(kv[0])
        ):
            for member_name, raw in version_entries.items():
                if group_name == "recovery_codes":
                    # Members author a sequence number; the 0x55 prefix is
                    # applied by ConstantsIndex — the shared derivation the
                    # emitter and the layout model already use.
                    members[member_name] = index.recovery_values[member_name]
                else:
                    entry = raw if isinstance(raw, dict) else {"value": raw}
                    members[member_name] = parse_int(entry["value"])
        enums[group_name] = {
            "underlying_type": group["underlying_type"],
            "members": members,
        }

    # Every named value from every value group (statically_defined_values, the
    # orientation constants, ...): each is an on-wire integer an encoder may
    # write, so each is a wire fact.
    constants = {
        name: parse_int(entry["value"] if isinstance(entry, dict) else entry)
        for name, entry in value_members(constants_doc).items()
    }

    return {"blocks": blocks, "entries": entries, "enums": enums, "constants": constants}


def witness_hash(
    fields_doc: dict[str, Any],
    constants_doc: dict[str, Any],
    header_doc: dict[str, Any],
) -> str:
    """sha256 of the canonical witness JSON — the wire contract's fingerprint.

    Emitted into every generated file's banner so --check can answer "was
    this produced from the current spec?" without comparing text (XP-2): the
    hash changes exactly when a wire fact changes, and not when an emitter
    comment does. The canonical form is the same (indent=2, sort_keys=True)
    rendering the committed baseline is written in.
    """
    canonical = json.dumps(
        witness(fields_doc, constants_doc, header_doc), indent=2, sort_keys=True
    )
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()
