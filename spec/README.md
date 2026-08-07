# IFE Specification Source (`spec/`)

The Iris File Extension is specified by three documents that together are the
**single source of truth** for the C++ serialization layer
(`generator/` → `generated_source/`), the LaTeX specification document, and
the HTML documentation (`generated_docs`):

| File | Role |
|------|------|
| `ife_fields.json` | **Byte structure only.** The type vocabulary and, for the preamble / universal block header / array header / every block: field names, types, and linkage — exactly what the generator needs to emit layouts. Field `description`s are permitted (they become comments in generated code) but nothing else narrative lives here. |
| `ife_constants.json` | **Values.** Statically defined values (sentinels) and every enumeration — recovery codes, tile encodings, pixel formats, metadata formats, annotation types, image encodings, orientations — with per-value descriptions and errata. |
| `ife_spec.md` | **The specification basis.** Hand-written narrative and all normative shall/should/may requirements, organized as the published document. `{{...}}` anchors mark where generated tables are interleaved at document build time. |

## Document organization (`// MARK:` banners)

JSON has no comment syntax, so both documents mark their section boundaries
with **banner keys** — the same `// MARK: -` convention the hand-written
headers use (`src/IrisCodecExtension.hpp`). A banner is a key beginning
`//`, whose value is a one-line description of the section that follows:

| Document | Sections |
|---|---|
| `ife_fields.json` | DOCUMENT IDENTITY · TYPE VOCABULARY · STRUCTURAL PREFIXES · BLOCK INVENTORY, the last subdivided into HEADER BLOCKS · TILE DATA ARRAYS · ATTRIBUTE ARRAYS · ASSOCIATED IMAGES · ICC COLOR PROFILE · ANNOTATION ARRAYS |
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
* Blocks are `kind: "header"` (fixed-size) or `kind: "array"`. Arrays define
  an `entry` (versioned `fields`, or `blob: true` for stride-1 byte arrays)
  and optionally versioned block-specific `header_fields` that follow the
  array header.
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

## `ife_spec.md` anchors

| Anchor | Renders |
|--------|---------|
| `{{preamble}}`, `{{block_header}}`, `{{array_header}}` | Shared structure layout tables |
| `{{layout:BLOCK}}` | Derived block layout table (parameter, type, derived offset, value/linkage) |
| `{{entry_layout:BLOCK}}` | Derived entry layout table for an array block |
| `{{constants:group}}` | Value table for a constants group |

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

* `ife.meta.schema.json` + CI validator: unique recovery tags, unique field
  names per block, `points_to`/`enum`/`constant` references resolve,
  append-only version groups, every block referenced by `{{layout:...}}`
  in `ife_spec.md`.
* v1 narrative not yet migrated into `ife_spec.md` (marked `TODO(draft)`):
  full definitions, MpP/Mc equations, global tile indexing equations and
  figures, slide-space figure.
