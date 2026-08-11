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
remainder duplicates SpecError paths the model already raises.
"""
# ---------------------------------------------------------------------------
# ROLE: checking only. Returns a list of human-readable problems; an empty
# list means consistent. Nothing here raises on a bad spec — the caller
# decides what a problem means, which is why the same function serves both
# `--validate` and the check that runs before every generation.
#
# Pure: reads the parsed documents, compares, appends strings. Modifies
# nothing and touches no files.
# ---------------------------------------------------------------------------

from __future__ import annotations

import re
from typing import Any, Iterator

from .model.layout import (
    _TYPE_WIDTH,
    RECOVERY_PREFIX,
    SpecError,
    constants_groups,
    is_banner,
    is_enum_group,
    parse_int,
    value_groups,
    value_members,
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
        k: v for k, v in constants_groups(constants_doc).items() if is_enum_group(v)
    }


def _members(group: dict[str, Any]) -> Iterator[tuple[str, str, Any]]:
    """(version, member name, raw entry) over an enum group."""
    for version, members in group.get("ife_version", {}).items():
        for name, raw in members.items():
            yield version, name, raw


# Lazy: produces one pair at a time and computes nothing until iterated, so a
# caller may walk it only once.
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
    return value_members(constants_doc).get(name, {})


def _narrative_anchors(narrative: str | None) -> set[str] | None:
    """Every `[[anchor]]` declared in the specification narrative.

    None when the narrative was not supplied, which is not an error: the C++
    layer generates from the JSON alone and must keep doing so.
    """
    if narrative is None:
        return None
    return set(re.findall(r"^\[\[([A-Za-z0-9_-]+)\]\]", narrative, re.M))


# Identifiers that are *macros* on a supported platform. A macro is expanded by
# the preprocessor before any namespace exists, so `offset::PLANES` becomes
# `offset::14` and the error names a line the schema never wrote. This cost a
# build once: PLANES is a GetDeviceCaps index in <wingdi.h>, and the field had
# to be renamed Z_PLANES after the fact.
#
# Types are not listed. `SIZE` is a struct in <windef.h>, not a macro, and a
# type is shadowed harmlessly by a namespaced constant of the same name — which
# is why the schema's several SIZE fields are fine and are not flagged here.
_PLATFORM_MACROS: dict[str, str] = {
    "PLANES": "<wingdi.h> (GetDeviceCaps index)",
    "BITSPIXEL": "<wingdi.h> (GetDeviceCaps index)",
    "NUMCOLORS": "<wingdi.h> (GetDeviceCaps index)",
    "COLORRES": "<wingdi.h> (GetDeviceCaps index)",
    "RASTERCAPS": "<wingdi.h> (GetDeviceCaps index)",
    "HORZRES": "<wingdi.h> (GetDeviceCaps index)",
    "VERTRES": "<wingdi.h> (GetDeviceCaps index)",
    "HORZSIZE": "<wingdi.h> (GetDeviceCaps index)",
    "VERTSIZE": "<wingdi.h> (GetDeviceCaps index)",
    "LOGPIXELSX": "<wingdi.h> (GetDeviceCaps index)",
    "LOGPIXELSY": "<wingdi.h> (GetDeviceCaps index)",
    "TRANSPARENT": "<wingdi.h> (background mode)",
    "ABSOLUTE": "<wingdi.h> (palette entry flag)",
    "RELATIVE": "<wingdi.h> (palette entry flag)",
    "DELETE": "<winnt.h> (access mask)",
    "ERROR": "<wingdi.h> (region complexity)",
    "OPTIONAL": "<winnt.h> (SAL annotation)",
    "IN": "<winnt.h> (SAL annotation)",
    "OUT": "<winnt.h> (SAL annotation)",
    "NEAR": "<minwindef.h>",
    "FAR": "<minwindef.h>",
    "NULL": "the C standard library",
    "EOF": "<stdio.h>",
    # <winbase.h> SetFilePointer move methods. FILE_END cost a second MSVC
    # build after PLANES cost the first, and it was absent from this table --
    # which is why the check now runs over sources as well as the schema.
    "FILE_BEGIN": "<winbase.h> (SetFilePointer origin)",
    "FILE_CURRENT": "<winbase.h> (SetFilePointer origin)",
    "FILE_END": "<winbase.h> (SetFilePointer origin)",
    "MAX_PATH": "<minwindef.h>",
    "TRUE": "<windef.h>",
    "FALSE": "<windef.h>",
    "NO_ERROR": "<winerror.h>",
    "interface": "<basetyps.h> (expands to struct)",
    "small": "<rpcndr.h> (expands to char)",
    "min": "<minwindef.h> (unless NOMINMAX)",
    "max": "<minwindef.h> (unless NOMINMAX)",
    "DOMAIN": "<math.h> (matherr type)",
    "CONSTANT": "<wingdi.h>",
}

# Schema field names are UPPER_SNAKE, so they are matched case-insensitively:
# a field named `domain` collides with the same macro as `DOMAIN`. C++ sources
# are matched exactly, because `min` and `Min` are different identifiers and
# only the first is the macro.
_PLATFORM_MACROS_UPPER: dict[str, str] = {k.upper(): k for k in _PLATFORM_MACROS}


def _field_names(fields_doc: dict[str, Any]) -> Iterator[tuple[str, str]]:
    """(where, field name) over every field the generator will emit."""
    for name, spec in _entries(fields_doc, "primitives").items():
        for _, field in _versioned_fields(spec.get("fields")):
            yield f"primitive {name}", field.get("name", "")
    for name, spec in _entries(fields_doc, "blocks").items():
        for _, field in _versioned_fields(spec.get("fields")):
            yield f"block {name}", field.get("name", "")
        entry = spec.get("entry")
        if entry:
            for _, field in _versioned_fields(entry.get("fields")):
                yield f"{name} entry", field.get("name", "")


def validate(
    fields_doc: dict[str, Any],
    constants_doc: dict[str, Any],
    document: dict[str, Any],
    narrative: str | None = None,
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
        # A block may decline a tag by declaring it null. The rule that every
        # block carries one exists because the recovery scan finds blocks by
        # tag; a block the scan finds another way needs no entry in a table
        # capped at 256 values. TILE_PIXEL_DATA is the case: its 40-bit
        # self-reference is unique to it, so the self-check identifies it and
        # a tag would be two bytes per tile that nothing reads.
        if tag is None:
            continue
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

    # A value group's members carry their own type: unlike an enumeration,
    # there is no group-wide underlying_type to inherit one from.
    for name, entry in value_members(constants_doc).items():
        if "type" not in entry:
            problems.append(f"named value {name!r} declares no type")
        elif entry["type"] not in types:
            problems.append(f"named value {name!r} has unknown type {entry['type']!r}")
        if "value" not in entry:
            problems.append(f"named value {name!r} declares no value")

    # One name, one meaning: two groups defining the same constant would emit
    # two C++ definitions of it, and the document would show it twice.
    seen_values: dict[str, str] = {}
    for group_name, group in value_groups(constants_doc).items():
        for entries in group.get("ife_version", {}).values():
            for name in entries:
                if name in seen_values and seen_values[name] != group_name:
                    problems.append(
                        f"named value {name!r} is defined in both "
                        f"{seen_values[name]!r} and {group_name!r}"
                    )
                seen_values[name] = group_name

    for group_name, group in groups.items():
        underlying = group.get("underlying_type")
        if underlying not in types:
            problems.append(f"enum {group_name}: unknown underlying_type {underlying!r}")
        elif _canonical(types, underlying) in ("f16", "f32", "f64"):
            problems.append(
                f"enum {group_name}: underlying_type {underlying!r} is floating point; "
                "C++ has no 'enum class : float'. A set of named values over a "
                "continuous domain belongs in a value group — one declaring no "
                "underlying_type — not in an enumeration."
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
    declared = [b["recovery_tag"] for b in blocks.values() if b.get("recovery_tag") is not None]
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

    _dv = document.get("version", {})
    ceiling_major = int(_dv.get("major", 0))
    ceiling_minor = int(_dv.get("minor", 0))

    # ---- 5b. the revision history ---------------------------------------- #
    # The version the document claims and the version its history records are
    # the same fact stated twice; this is what stops them disagreeing. And a
    # ratified entry without a date publishes an authority claim nobody can
    # check, which is worse than an unratified one that says so.
    revisions = document.get("revisions", [])
    if not revisions:
        problems.append("ife_header.json declares no revisions; every published version needs one")
    seen_versions: set[str] = set()
    for entry in revisions:
        label = entry.get("version")
        if label is None:
            problems.append("a revision states no version")
            continue
        if label in seen_versions:
            problems.append(f"revision {label!r} is listed more than once")
        seen_versions.add(label)
        if not entry.get("authors"):
            problems.append(f"revision {label!r} names no author")
        if not entry.get("summary"):
            problems.append(f"revision {label!r} summarises no changes")
        if entry.get("status") == "ratified" and not entry.get("date"):
            problems.append(
                f"revision {label!r} is marked ratified but states no date; a ratified "
                "version is ratified on a day, and the document has to say which"
            )
        date = entry.get("date")
        # Month precision is legitimate -- a specification is ratified in a
        # month people remember, and a day nobody verified is worse than none.
        if date and not re.fullmatch(r"\d{4}-\d{2}(-\d{2})?", str(date)):
            problems.append(
                f"revision {label!r} has date {date!r}; use ISO 8601 "
                "(YYYY-MM-DD, or YYYY-MM where only the month is known)"
            )

    current = f"{ceiling_major}.{ceiling_minor}" if revisions else None
    if revisions and current not in seen_versions:
        problems.append(
            f"the document is version {current} but the revision history has no entry for "
            f"it (found: {', '.join(sorted(seen_versions))})"
        )

    # Both anchor checks below need the narrative's declared anchors.
    anchors = _narrative_anchors(narrative)

    # ---- 6a. every block has a section for a points_to link to reach ----- #
    # The layout tables render each points_to field as a cross-reference to the
    # target block's section. A block whose section is missing an anchor turns
    # that link into an unresolved xref in the published document, which
    # Asciidoctor reports and then renders as literal text.
    if anchors is not None:
        for block_name in blocks:
            expected = "ife-" + block_name.lower().replace("_", "-")
            if expected not in anchors:
                problems.append(
                    f"block {block_name!r} has no section anchored {expected!r} in the "
                    "narrative; its layout table's cross-references cannot resolve"
                )

    # ---- 6. normative clauses point at a clause the document declares ---- #
    # A clause cites a stable anchor rather than a section number, and this is
    # what makes that worth anything: a dangling or renamed anchor fails here
    # instead of rendering a diagnostic and a document that confidently cite a
    # requirement nobody can find. Section numbers were tried and are worse --
    # a renumbering that lands on another real section is undetectable.
    if anchors is not None:
        for owner, spec in _every_versioned(fields_doc, blocks, primitives):
            for _, field in _versioned_fields(spec):
                clause = (field.get("conformance") or {}).get("clause")
                if clause is None:
                    continue
                if clause not in anchors:
                    problems.append(
                        f"{owner}.{field.get('name', '<unnamed>')}: normative clause cites "
                        f"{clause!r}, which the specification narrative does not anchor"
                    )

    ceiling = (ceiling_major, ceiling_minor)
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
    # ---- 6. identifiers that a platform defines as macros --------------- #
    # Cheap, and the alternative is finding out from a Windows build.
    for where, name in _field_names(fields_doc):
        # Case-insensitive: schema names are UPPER_SNAKE, so `PLANES` and a
        # hypothetical `Planes` collide with the same macro.
        canonical = _PLATFORM_MACROS_UPPER.get(name.upper())
        if canonical:
            problems.append(
                f"{where}: field {name!r} is a macro in {_PLATFORM_MACROS[canonical]}. "
                "The preprocessor expands it before namespaces apply, so the "
                "generated `offset::" + name + "` becomes whatever the macro "
                "says and the compiler reports a line this schema never wrote. "
                "Rename the field."
            )
    for group_name, group in _enum_groups(constants_doc).items():
        for _, member, _raw in _members(group):
            canonical = _PLATFORM_MACROS_UPPER.get(member.upper())
            if canonical:
                problems.append(
                    f"enum {group_name}: member {member!r} is a macro in "
                    f"{_PLATFORM_MACROS[canonical]}; rename it."
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
