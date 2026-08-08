# IFE Specification Source (`spec/`)

The Iris File Extension is specified by four documents that together are the
**single source of truth** for the C++ serialization layer
(`generator/` → `generated_source/`), the LaTeX specification document, and
the HTML documentation (`generated_docs`):

| File | Role |
|------|------|
| `ife_header.json` | **Specification identity.** Name, version, status, copyright, licence — stated **once**. It belongs to neither the layout nor the constants, and a version or copyright written in two files is one that can disagree with itself. Generated file banners and `IFE_SCHEMA_VERSION_MAJOR`/`_MINOR` both come from here. |
| `ife_fields.json` | **Byte structure only.** The type vocabulary, the primitive block types, and for every block: field names, types, and linkage — exactly what the generator needs to emit layouts. Field `description`s are permitted (they become comments in generated code) but nothing else narrative lives here. |
| `ife_constants.json` | **Values.** Statically defined values (sentinels) and every enumeration — recovery codes, tile encodings, pixel formats, metadata formats, annotation types, image encodings, orientations — with per-value descriptions and errata. |
| `ife_spec.md` | **The specification basis.** Hand-written narrative and all normative shall/should/may requirements, organized as the published document. `{{...}}` markers show where generated tables belong. **Provisional:** Phase 5 converts this file to AsciiDoc (`ife_spec.adoc`) and replaces each marker with Asciidoctor's native `include::` — see MIGRATION.md Phase 5 for why a bespoke anchor syntax was rejected. |

## What may never enter the schema

These documents describe **what a byte is**. They never describe what a
program should **do**. The boundary is fixed; treat a proposal to widen it as
a design error rather than a feature request.

**Allowed:** field name, scalar type or enum reference, introducing version,
offset linkage (`points_to`/`nullable`), a constant a field always holds, the
primitive a block derives from, its recovery tag, documentation
(`description`, `errata`, `deprecated`), and validation predicates from a
closed vocabulary — range, enum membership, ordering, non-null.

**Never:** expressions or arithmetic · conditionals of any kind (no field may
depend on another field's value for its presence, type or width) · computed
lengths (a length is a field, or `STRIDE`/`COUNT`) · literal offsets or sizes
· alignment or padding directives · byte-order overrides · anything about
runtime behaviour · any type whose width is not derivable from the schema
alone.

A rule that will not fit belongs in `ife_spec.adoc` as prose, hand-written
into the validation layer. That is the intended escape valve. If a proposal
needs `if`, the answer is no.

## Checking the documents

`python3 -m generator --validate` runs before every generation and in CI. It
catches what layout derivation does not care about but a *file* does:

* **Recovery-tag conflicts** — two blocks sharing a tag, an unknown tag, a tag
  no block claims, and the ceiling: the sequence is the low byte of `0x55xx`,
  so **256 tags is the hard limit**. Tag 256 would be `0x5600`, changing the
  prefix that keeps the corruption scan's false-positive rate acceptable.
  Passing that point is a decision to admit a second prefix, not a routine
  addition.
* **Enum value conflicts** — two members aliasing one value.
* **Types** — every `type` and `underlying_type` resolves through the alias
  chain to something the generator can emit, declared widths agree with the
  emitted ones, no alias cycles, and no floating-point enum (C++ has no
  `enum class : float`).
* **Dangling references** — `points_to`, `enum`, `primitive`, `extends`.
* **Ordering** — block order matches recovery-tag order. Tag values are
  positional and therefore on the wire; a reorder redefines every file already
  written, and this is the check that makes deriving them safe.

Recovery-tag **values are never authored**. Each is `0x5500` plus its position,
assigned by the generator. Writing one by hand is itself an error.

## Saying "we are not specifying this"

A blank in a specification is ambiguous: not decided yet, decided not to
decide, or forgotten. Mark the second case explicitly with `unspecified` on a
field or block — one of three values, always with prose saying why:

* `reserved` — the slot is claimed, encoders write nothing, meaning is
  deferred to a future version (`CIPHER`).
* `external` — another authority defines these bytes; add `specified_by`
  naming it (`ICC_PROFILE` → the ICC specification; `ANNOTATION_BYTES` →
  whichever `annotation_types` value the entry declares).
* `implementation` — the encoder chooses; IFE declines to constrain it.

It changes no layout: the field keeps its type, width and offset. It tells
the validation layer there is nothing here to enforce, and it makes the
published document say "Reserved" or "Not specified by this document" instead
of leaving a cell empty.

## Strings

**There is no string type.** Every string in IFE is a byte range whose length
comes from a size field elsewhere — never null-terminated, never
length-prefixed in place. Keys and values live in `ATTRIBUTE_BYTES` sliced by
`ATTRIBUTE_SIZES`; group titles live in `ANNOTATION_GROUP_BYTES` sliced by
`ANNOTATION_GROUP_SIZES`; an image label is the first `TITLE_SIZE` bytes of
`IMAGE_BYTES`. A string is therefore a **byte array** (stride 1), and its
character encoding — ASCII for keys, labels and titles; UTF-8 for attribute
values — is normative prose in `ife_spec.md`, not layout.

Do not add a `string` entry to the `types` table. A layout type must have a
width derivable from the schema alone, and a string does not: it would have
to invent either a length prefix or a terminator, neither of which IFE has.

## Document organization (`// MARK:` banners)

JSON has no comment syntax, so both documents mark their section boundaries
with **banner keys** — the same `// MARK: -` convention the hand-written
headers use (`src/IrisCodecExtension.hpp`). A banner is a key beginning
`//`, whose value is a one-line description of the section that follows:

| Document | Sections |
|---|---|
| `ife_fields.json` | DOCUMENT IDENTITY · TYPE VOCABULARY · PRIMITIVE BLOCK TYPES · BLOCK INVENTORY, the last subdivided into HEADER BLOCKS · TILE DATA ARRAYS · ATTRIBUTE ARRAYS · ASSOCIATED IMAGES · ICC COLOR PROFILE · ANNOTATION ARRAYS |
| `ife_constants.json` | DOCUMENT IDENTITY · STATICALLY DEFINED VALUES · STRUCTURAL ENUMERATIONS · CONTENT ENUMERATIONS |

Every consumer skips them via `generator.model.layout.is_banner()`, which is
the single definition of the rule — the layout model and both emitters cannot
disagree about what a banner is. Banners are documentation only: adding,
editing or removing one **cannot** change a byte of generated output. If it
ever does, that is a bug in the skip rule.

Keep a banner's key unique within its object (JSON forbids duplicate keys)
and keep the block grouping aligned with the specification document's own
section order, so a reader moving between the two is never re-orienting.

## `ife_fields.json` schema

* Every field list is keyed by introducing IFE version
  (`fields.ife_version.{"1.0": [...], "1.1": [...]}`); layout derivation
  concatenates groups in ascending version order and computes offsets from
  cumulative field widths. **No offset or size is ever stated.**
* A field is `{name, type | enum, [constant], [points_to, nullable],
  [description]}`:
  * `type` — a scalar from the `types` table.
  * `enum` — a group in `ife_constants.json`; the field's width derives from
    that group's `underlying_type`. **Use an enum wherever the value domain
    is enumerable**; a raw `type` is only for genuinely free values.
  * `constant` — the field always holds this statically defined value.
  * `points_to` + `nullable` — offset linkage to another block and whether
    NULL_OFFSET is permitted.
* Every block names the **primitive** it derives from, and contributes its
  own versioned `fields` after that primitive's prefix:

  | Primitive | Extends | Adds | Prefix |
  |---|---|---|---|
  | `file_header` | — | `MAGIC`, `RECOVERY` | 6 B |
  | `block` | — | `VALIDATION`, `RECOVERY` | 10 B |
  | `array` | `block` | `STRIDE`, `COUNT` | 16 B |
  | `byte_array` | `block` | `COUNT` (byte count) | 14 B |

  `file_header` carries no `VALIDATION` because the root is at byte 0, where
  that field could only ever store zero. `byte_array` carries no `STRIDE`
  because a byte's stride is intrinsically 1. Neither is an exception to be
  tidied away — they are why the primitives are declared separately.
  An `array` block also defines an `entry` with its own versioned `fields`.
* Append-only rule: a new minor version adds a new `ife_version` group;
  existing groups are never edited or reordered. All fields currently live
  under `"1.0"`; `"1.1"` and later are reserved for append-only additions.

## `ife_constants.json` schema

Two kinds of top-level entry, and the distinction is load-bearing:

* **Enumerations** — `{description, underlying_type, ife_version: {version →
  members}}`, emitted as `enum class <PascalName> : <underlying>`. A member is
  `NAME: {value, description, [errata]}`, or the bare form `NAME: value` where
  no documentation is carried. *Prefer the object form:* the bare form cannot
  describe itself, and Phase 5 renders these tables into the published
  document. `recovery_codes` is currently the only group still using it.
* **Statically defined values** — sentinels and named convenience constants,
  each carrying its **own** `type` rather than sharing an `underlying_type`,
  emitted as `inline constexpr`. This is where a set of named values over a
  *continuous* domain belongs: the `ORIENTATION_*` degree constants live here,
  not in an enumeration, because any degree value is legal and C++ has no
  `enum class : float`. Reach for an enumeration only when the value domain is
  genuinely closed.

## `ife_spec.md` insertion markers (provisional)

These mark where generated content belongs. They are **placeholders, not a
supported syntax** — no preprocessor implements them, and Phase 5 replaces
each with an Asciidoctor `include::` of the corresponding generated file. Do
not build tooling against them.

| Marker | Will include |
|--------|--------------|
| `{{preamble}}`, `{{block_header}}`, `{{array_header}}` | Shared structure layout tables |
| `{{layout:BLOCK}}` | Derived block layout table (parameter, type, derived offset, value/linkage) |
| `{{entry_layout:BLOCK}}` | Derived entry layout table for an array block |
| `{{constants:group}}` | Value table for a constants group |

Keep one marker per generated table rather than one per section: Phase 5
emits fine-grained include files so the narrative pulls in exactly what it
needs, and moving a section never drags unrelated tables along.

## Design decisions embedded in this draft (awaiting ratification)

1. **Universal block header, no exceptions** — every block starts
   `VALIDATION (u64) | RECOVERY (u16)`; the file header conforms (its
   validation value is always 8).
2. **8-byte version-invariant preamble** (`MAGIC | SPEC_MAJOR | SPEC_MINOR`);
   v1 files detected by bytes 4–5 reading 0x5501.
3. **Spec version lives in the preamble**, not the file header;
   `FILE_REVISION` stays in the header.
4. **`FILE_SIZE` doubles as the atomic write-head** during encoding.
5. **Uniform array header** (`STRIDE u16 | COUNT u32`) on every array block,
   blobs included (stride 1); block-specific `header_fields` always follow it.
6. **Recovery tags keep v1 values and the 0x55 entropy convention**;
   explicit in JSON, uniqueness enforced by the meta-schema.
7. **Packed widths retained** (`u40`/`u24` tile entries) as a deliberate
   density feature.
8. **Sentinel errata corrected** (`NULL_TILE` → 40-bit max, `NULL_ID` →
   24-bit max).
9. **`METADATA_FREE_TEXT` gets a distinct value (3).**
10. **`TILE_PIXEL_DATA` stays unframed** — specified in `ife_spec.md`
    Section 4.3 only; it has no byte structure to generate.

Derived layout consequences (computed, never stored): preamble 8 B;
universal header 10 B; array header 16 B total; `FILE_HEADER` 38 B (46 B from
SOF); `TILE_TABLE` 44 B; `METADATA` 56 B; `ATTRIBUTES` 29 B; strides — layer
extent 12, tile offset 8, attribute size 6, image entry 20, annotation
entry 39, group size 6.

## Not yet present (later phases)

* **The append-only check.** `--validate` catches conflicts and dangling
  references *within* one revision; nothing yet compares a revision against
  its predecessor, which is the only way to catch a `1.0` field being moved,
  resized or retyped. It is a property of the **diff**, so no check over a
  single document can see it. See MIGRATION.md Phase 2, validator item 1.
  (A `ife.meta.schema.json` meta-schema was written and **deleted** — JSON
  Schema cannot express this check, nor uniqueness over object values, nor
  cross-document agreement, and it would add a dependency to a deliberately
  stdlib-only toolchain. Do not re-propose it.)
* **Normative clause tagging.** No field carries a shall/should/may predicate
  yet; MIGRATION.md 4.6 step 2 adds them, drawn from the capped vocabulary.
* v1 narrative not yet migrated into `ife_spec.md` (marked `TODO(draft)`):
  full definitions, MpP/Mc equations, global tile indexing equations and
  figures, slide-space figure.
