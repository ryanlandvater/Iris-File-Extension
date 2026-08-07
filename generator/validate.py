"""Consistency checks over the IFE specification documents.

Layout derivation (generator/model) already rejects what it cannot compute:
unknown types, alias cycles, a duplicate field name inside one list. This
module checks what derivation does not care about but a *file* does —
conflicts and dangling references that would produce a well-formed generator
run and an unreadable slide.

The checks are ordered by how much damage the defect causes:

  1. Recovery-tag conflicts. Two blocks sharing a tag makes the corruption
     scan ambiguous exactly when it is needed most, and nothing else in the
     pipeline notices.
  2. Enum value conflicts. Two members with one value silently alias, so a
     reader reports whichever name came second.
  3. Dangling references. A points_to, enum or primitive naming something
     that does not exist.
  4. Document agreement. The two spec files must describe one specification.
  5. Convention. Recovery tags are the 0x55 sequence; version labels parse.

This is not a JSON Schema, deliberately: checks 1-4 are inexpressible in one
(uniqueness over object values, cross-document agreement), and the structural
remainder duplicates SpecError paths the model already raises. See
MIGRATION.md Phase 2.
"""
# ---------------------------------------------------------------------------
# ROLE: checking only. Returns a list of human-readable problems; an empty
# list means consistent. Nothing here raises on a bad spec — the caller
# decides what a problem means, which is why the same function serves both
# `--validate` and the check that runs before every generation.
#
# For a C++ reader: this is a pure predicate over the parsed documents. It
# reads, compares, and appends strings. It never modifies the documents and
# never touches the filesystem.
# ---------------------------------------------------------------------------

from __future__ import annotations

from typing import Any, Iterator

from .model.layout import (
    _TYPE_WIDTH,
    RECOVERY_PREFIX,
    SpecError,
    is_banner,
    parse_int,
    version_key,
)

# The tag prefix occupies the high byte, so the sequence occupies the low one:
# 0x5500 through 0x55FF. Tag 256 would be 0x5600 and would silently change the
# prefix that makes the recovery scan's false-positive rate acceptable.
MAX_RECOVERY_TAGS = 0x100


def _entries(doc: dict[str, Any], key: str) -> dict[str, Any]:
    """Non-banner children of a mapping."""
    return {k: v for k, v in doc.get(key, {}).items() if not is_banner(k)}


def _versioned_fields(spec: dict[str, Any] | None) -> Iterator[tuple[str, dict[str, Any]]]:
    """(version, field) over a versioned field list, in document order."""
    for version, fields in (spec or {}).get("ife_version", {}).items():
        for field in fields:
            yield version, field


def _enum_groups(constants_doc: dict[str, Any]) -> dict[str, Any]:
    return {
        k: v
        for k, v in constants_doc.items()
        if k not in ("$schema", "spec", "statically_defined_values") and not is_banner(k)
    }


def _members(group: dict[str, Any]) -> Iterator[tuple[str, str, Any]]:
    """(version, member name, raw entry) over an enum group."""
    for version, members in group.get("ife_version", {}).items():
        for name, raw in members.items():
            yield version, name, raw


# `yield` makes this a generator: it produces one pair at a time and computes
# nothing until iterated. Equivalent to returning a lazy range. The caller can
# only walk it once.
def _member_values(group_name: str, group: dict[str, Any]) -> Iterator[tuple[str, int]]:
    """(member name, value) for an enum group, deriving where values are derived.

    recovery_codes states no values: each is RECOVERY_PREFIX plus its position,
    assigned identically here, in the emitter and in ConstantsIndex. Every
    other group authors its values.
    """
    for _, name, raw in _members(group):
        value = raw.get("value") if isinstance(raw, dict) else raw
        if value is None:
            # A member with no value is reported by the checks that own that
            # rule; skipping keeps this a value-conflict scan rather than a
            # second place that decides what a well-formed member looks like.
            continue
        try:
            number = parse_int(value)
        except SpecError:
            continue  # malformed value: reported where the type is checked
        yield name, RECOVERY_PREFIX + number if group_name == "recovery_codes" else number


def _canonical(types: dict[str, Any], name: str) -> str:
    """Resolve an alias chain to its canonical scalar, tolerating a broken chain."""
    seen: set[str] = set()
    while name in types and "alias" in types[name] and name not in seen:
        seen.add(name)
        name = types[name]["alias"]
    return name


def _sentinel(constants_doc: dict[str, Any], name: str) -> dict[str, Any]:
    for entries in constants_doc.get("statically_defined_values", {}).get("ife_version", {}).values():
        if name in entries:
            return entries[name]
    return {}


def validate(
    fields_doc: dict[str, Any],
    constants_doc: dict[str, Any],
    document: dict[str, Any],
) -> list[str]:
    """Return a list of problems; empty means the documents are consistent."""
    problems: list[str] = []
    blocks = _entries(fields_doc, "blocks")
    primitives = _entries(fields_doc, "primitives")
    groups = _enum_groups(constants_doc)

    # ---- 1. recovery-tag conflicts ------------------------------------ #
    recovery = groups.get("recovery_codes", {})
    tag_value: dict[str, int] = {}
    value_owner: dict[int, str] = {}
    declared_tags = sum(len(v) for v in recovery.get("ife_version", {}).values())
    if declared_tags > MAX_RECOVERY_TAGS:
        problems.append(
            f"{declared_tags} recovery tags declared; the sequence cannot exceed "
            f"{MAX_RECOVERY_TAGS} ({RECOVERY_PREFIX:#06x}-0x55FF). Tag "
            f"{MAX_RECOVERY_TAGS} would be 0x5600, changing the high byte that "
            "keeps a recovery scan's false-positive rate acceptable. Going past "
            "this means deliberately admitting a second prefix, not adding one "
            "more block."
        )
    for index, (_, name, raw) in enumerate(_members(recovery)):
        entry = raw if isinstance(raw, dict) else {"value": raw}
        if "value" not in entry:
            problems.append(
                f"recovery tag {name!r} states no sequence number; each tag "
                "declares its position and the generator adds the 0x55 prefix"
            )
            continue
        sequence = entry["value"]
        if not isinstance(sequence, int):
            problems.append(
                f"recovery tag {name!r} states {sequence!r}; this field is the "
                "sequence NUMBER (an integer), not the tag value — the "
                f"{RECOVERY_PREFIX:#06x} prefix is added by the generator"
            )
            continue
        if not 0 <= sequence < MAX_RECOVERY_TAGS:
            problems.append(
                f"recovery tag {name!r} has sequence number {sequence}, outside "
                f"0-{MAX_RECOVERY_TAGS - 1}. The sequence is the low byte of "
                f"{RECOVERY_PREFIX:#06x}; {MAX_RECOVERY_TAGS} would be 0x5600 and "
                "change the prefix that keeps a recovery scan's false-positive "
                "rate acceptable."
            )
        if sequence != index:
            problems.append(
                f"recovery tag {name!r} has sequence number {sequence} but sits at "
                f"position {index}. Numbers are issued in order and never "
                "reissued: a gap or a swap means one was reused or a tag was "
                "removed, and either redefines files already written."
            )
    for name, value in _member_values("recovery_codes", recovery):
        if name in tag_value:
            problems.append(f"recovery tag {name!r} is declared more than once")
        if value in value_owner and value_owner[value] != name:
            problems.append(
                f"recovery tag conflict: {name!r} and {value_owner[value]!r} "
                f"both use {value:#06x} — a corruption scan cannot tell them apart"
            )
        tag_value[name] = value
        value_owner[value] = name

    declared_names = [name for _, name, _ in _members(recovery)]  # document order
    claimed: dict[str, str] = {}
    for block_name, block in blocks.items():
        tag = block.get("recovery_tag")
        if tag not in declared_names:
            problems.append(f"block {block_name!r} claims unknown recovery tag {tag!r}")
            continue
        if tag in claimed:
            problems.append(
                f"recovery tag {tag!r} is claimed by both {claimed[tag]!r} and "
                f"{block_name!r}; each block needs its own"
            )
        claimed[tag] = block_name
    for name in declared_names:
        if name not in claimed and name != "RECOVER_UNDEFINED":
            problems.append(f"recovery tag {name!r} is declared but no block claims it")

    # ---- 2. enum value conflicts -------------------------------------- #
    for group_name, group in groups.items():
        seen_value: dict[int, str] = {}
        seen_name: set[str] = set()
        for _, name, raw in _members(group):
            entry = raw if isinstance(raw, dict) else {"value": raw}
            if "value" not in entry:
                problems.append(f"enum {group_name}: member {name!r} states no value")
        for name, value in _member_values(group_name, group):
            if name in seen_name:
                problems.append(f"enum {group_name}: member {name!r} declared more than once")
            seen_name.add(name)
            if value in seen_value:
                problems.append(
                    f"enum {group_name}: {name!r} and {seen_value[value]!r} both use "
                    f"{value:#x} — a reader cannot distinguish them"
                )
            seen_value[value] = name

    # ---- 3. dangling references --------------------------------------- #
    types = fields_doc.get("types", {})
    for block_name, block in blocks.items():
        primitive = block.get("primitive")
        if primitive not in primitives:
            problems.append(f"block {block_name!r} names unknown primitive {primitive!r}")

        sources = [("", block.get("fields"))]
        entry = block.get("entry")
        if entry:
            sources.append((f".{entry.get('name', 'ENTRY')}", entry.get("fields")))

        for suffix, spec in sources:
            names: set[str] = set()
            for _, field in _versioned_fields(spec):
                fname = field.get("name", "<unnamed>")
                where = f"{block_name}{suffix}.{fname}"
                if fname in names:
                    problems.append(f"{where}: field name declared more than once")
                names.add(fname)
                target = field.get("points_to")
                if target is not None and target not in blocks:
                    problems.append(f"{where}: points_to unknown block {target!r}")
                enum = field.get("enum")
                if enum is not None and enum not in groups:
                    problems.append(f"{where}: references unknown enum group {enum!r}")
                type_name = field.get("type")
                if type_name is not None and type_name not in types:
                    problems.append(f"{where}: unknown type {type_name!r}")

    for prim_name, prim in primitives.items():
        parent = prim.get("extends")
        if parent is not None and parent not in primitives:
            problems.append(f"primitive {prim_name!r} extends unknown primitive {parent!r}")

    # ---- 3b. the type vocabulary itself --------------------------------- #
    for type_name, spec in types.items():
        if is_banner(type_name):
            continue
        if "alias" in spec:
            seen_alias: set[str] = set()
            target, chain = spec["alias"], [type_name]
            while True:
                if target in seen_alias:
                    problems.append(f"type alias cycle: {' -> '.join(chain + [target])}")
                    break
                seen_alias.add(target)
                chain.append(target)
                entry = types.get(target)
                if entry is None:
                    problems.append(f"type {type_name!r} aliases unknown type {target!r}")
                    break
                if "alias" not in entry:
                    if target not in _TYPE_WIDTH:
                        problems.append(
                            f"type {type_name!r} resolves to {target!r}, which the "
                            "generator cannot emit"
                        )
                    break
                target = entry["alias"]
        elif type_name not in _TYPE_WIDTH:
            problems.append(
                f"type {type_name!r} is not an alias and is not one the generator "
                f"can emit (known: {', '.join(sorted(_TYPE_WIDTH))})"
            )
        elif "bits" not in spec:
            problems.append(f"type {type_name!r} declares no 'bits'")
        elif spec["bits"] != _TYPE_WIDTH[type_name] * 8:
            problems.append(
                f"type {type_name!r} declares {spec['bits']} bits but the generator "
                f"emits it as {_TYPE_WIDTH[type_name] * 8}"
            )

    sentinels = {
        name
        for entries in constants_doc.get("statically_defined_values", {})
        .get("ife_version", {})
        .values()
        for name in entries
    }
    for name in sentinels:
        entry = _sentinel(constants_doc, name)
        if "type" not in entry:
            problems.append(f"statically defined value {name!r} declares no type")
        elif entry["type"] not in types:
            problems.append(f"statically defined value {name!r} has unknown type {entry['type']!r}")
        if "value" not in entry:
            problems.append(f"statically defined value {name!r} declares no value")

    for group_name, group in groups.items():
        underlying = group.get("underlying_type")
        if underlying is None:
            problems.append(f"enum {group_name}: no underlying_type")
        elif underlying not in types:
            problems.append(f"enum {group_name}: unknown underlying_type {underlying!r}")
        elif _canonical(types, underlying) in ("f16", "f32", "f64"):
            problems.append(
                f"enum {group_name}: underlying_type {underlying!r} is floating point; "
                "C++ has no 'enum class : float'. A set of named values over a "
                "continuous domain belongs in statically_defined_values."
            )

    # ---- 4. the specification document ---------------------------------- #
    for required in ("name", "version", "status", "copyright", "license"):
        if required not in document:
            problems.append(f"ife_header.json declares no {required!r}")
    version = document.get("version", {})
    if not isinstance(version, dict) or "major" not in version or "minor" not in version:
        problems.append("ife_header.json: version must be {major, minor}")
    for stale in ("spec",):
        if stale in fields_doc or stale in constants_doc:
            problems.append(
                f"{stale!r} appears in a schema document; specification identity "
                "lives only in ife_header.json"
            )


    # ---- 5. conventions ------------------------------------------------ #
    # Tag values are positional, so the ORDER of this list is on the wire.
    # Blocks must appear in the same order as the tags they claim, and neither
    # may be reordered: doing so silently redefines what every shipped file
    # says. This check is what makes derivation safe rather than clever.
    declared = [b.get("recovery_tag") for b in blocks.values()]
    sequence = [n for n in declared_names if n != "RECOVER_UNDEFINED"]
    if declared != sequence:
        for index, (got, want) in enumerate(zip(declared, sequence)):
            if got != want:
                problems.append(
                    f"block order diverges from recovery-tag order at position "
                    f"{index}: the block claims {got!r} where the tag sequence has "
                    f"{want!r}. Tag values are positional and on the wire — a "
                    "reorder redefines every file already written."
                )
                break
        if len(declared) != len(sequence):
            problems.append(
                f"{len(blocks)} blocks but {len(sequence)} block-claimable recovery "
                "tags; every block needs exactly one and every tag exactly one block"
            )

    document_version = document.get("version", {})
    ceiling = (int(document_version.get("major", 0)), int(document_version.get("minor", 0)))
    for owner, spec in _every_versioned(fields_doc, blocks, primitives):
        for label in (spec or {}).get("ife_version", {}):
            try:
                key = version_key(label)
            except ValueError:
                problems.append(f"{owner}: malformed version label {label!r}")
                continue
            if key > ceiling:
                problems.append(
                    f"{owner}: version group {label!r} is newer than the document "
                    f"version {ceiling[0]}.{ceiling[1]}"
                )
    return problems


def _every_versioned(
    fields_doc: dict[str, Any], blocks: dict[str, Any], primitives: dict[str, Any]
) -> Iterator[tuple[str, dict[str, Any] | None]]:
    """Every versioned field list in the document, with a label for messages."""
    for name, prim in primitives.items():
        yield f"primitive {name}", prim.get("fields")
    for name, block in blocks.items():
        yield f"block {name}", block.get("fields")
        entry = block.get("entry")
        if entry:
            yield f"block {name} entry", entry.get("fields")
