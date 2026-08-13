# Phase 6 — Conformance corpus

> **The cutover checklist is in `MIGRATION.md` §"Phase 6 execution plan —
> retire `src/IrisCodecExtension.*`" (2026-08-12), and is newer than this
> document.** This file covers what the fixtures must *contain*; that one
> covers how v1 is removed. Read both, and where they disagree, follow
> MIGRATION.md — in particular, deleting v1 is **not** blocked on this corpus:
> a snapshot of v1's own output — hosted on `iris.exampleslides.org`,
> digest-pinned in the committed manifest (MIGRATION 6.3), never committed as
> a blob — preserves the oracle without it.
> Reviewed 2026-08-12: the five-block gap list below was re-verified — it maps
> to exactly 10 unexecuted generated functions, and the oracle's 13/18 block
> count was confirmed empirically (2,857 B fixture; SHA-256 recorded in
> MIGRATION 6.3).

Refinement pass for Phase 6. The corpus exists to replace what
`ife_v1_oracle_tests` proves: that the generated layer reads bytes **a shipped
encoder wrote**, rather than agreeing with another description of the format.
Nothing else in the suite can settle that, and until the corpus covers what the
oracle covers, `src/IrisCodecExtension.*` cannot be retired.

## Why the published example files are not enough

Measured against `Iris-Example-Files@main`, both files (`425248_JPEG.iris`,
`425248_AVIF.iris`) are the same specimen and structurally identical:

| | |
|---|---|
| Version | IFE 1.0, revision 0 |
| Size | 1,718,162,733 / 1,123,383,971 B |
| Layers | 4 (8×6, 31×22, 124×87, 496×346 tiles) |
| Tile offsets | 183,134 entries, **0** `NULL_TILE` slots |
| Structural bytes | 1,465,252 B — the last 0.09% of the file |
| `ATTRIBUTES`, `IMAGES`, `ICC_PROFILE`, `ANNOTATIONS` | all `NULL` |
| `MICRONS_PIXEL`, `MAGNIFICATION` | 0 |

They exercise **6 of 18 block types**. Twelve never appear, including every
block the oracle covers on the metadata side. A corpus built from them would
reduce real-byte coverage rather than replace it.

## What the fixtures must contain

Purpose-built `.test_slide` files, encoded by the shipped encoder. **Small by
construction** — the excerpt machinery a 1.7 GB slide would need is only
necessary because those files are 99.9% pixel data. A fixture needs a valid
pyramid, not a large one: 8×6 tiles at layer 0 and two further layers is under
5 MB with real JPEG tiles, and the whole hosting and CI problem shrinks with it.

Every block must appear at least once across the set. Blocks marked ⚠ appear in
**no** currently published file and are the reason this work exists.

| Block | How it is made to appear |
|---|---|
| `FILE_HEADER`, `TILE_TABLE`, `LAYER_EXTENTS`, `TILE_OFFSETS`, `TILE_PIXEL_DATA` | any slide |
| `METADATA` | always present; populate `MICRONS_PIXEL` and `MAGNIFICATION` non-zero ⚠ |
| `ATTRIBUTES`, `ATTRIBUTE_SIZES`, `ATTRIBUTE_BYTES` ⚠ | ≥1 attribute key-value pair; one fixture per `metadata_formats` value that has meaning (`METADATA_I2S`, `METADATA_DICOM`) |
| `IMAGES`, `IMAGE_BYTES` ⚠ | ≥2 associated images (label + thumbnail), differing in `ENCODING` and `ORIENTATION` |
| `ICC_PROFILE` ⚠ | any embedded profile |
| `ANNOTATIONS`, `ANNOTATION_BYTES` | ≥1 of each `annotation_types` value — `ANNOTATION_PNG`, `JPEG`, `SVG`, `TEXT`. **Unblocked**, see below |
| `ANNOTATION_GROUP_SIZES`, `ANNOTATION_GROUP_BYTES` ⚠ | ≥1 named annotation group — **still blocked: v1 has no group writer**, see below |
| `CIPHER` ⚠ | one encrypted fixture, if the encoder can write one; otherwise record it as permanently uncovered |
| `CLINICAL_METADATA` ⚠ | 1.1 only — one fixture per `clinical_encodings` value the encoder supports |

## Annotations: unblocked (2026-08-12)

`ANNOTATIONS` and `ANNOTATION_BYTES` were listed here as impossible to cover by
oracle, because the shipped encoder could not write them and its reader could
not return them. Seven defects, fixed in `62eaeeb`; the first masked the rest,
which is why none had been found.

| | Defect | Effect |
|---|---|---|
| A | `METADATA::validate_full` built the `ANNOTATIONS` handle from `IMAGES_OFFSET` | any file with annotations failed validation |
| B | `AnnotationInfo` in a `std::set` with no `operator<` | the writer could not be given entries — a compile-time wall |
| C | `STORE_ANNOTATION_ARRAY` left both group offsets unwritten | `groups()` read 0 as a pointer and claimed groups that did not exist |
| D | `ANNOTATIONS::validate_full` read the group pointers without `__offset` | reached absolute offsets 16 and 24, inside `FILE_HEADER` |
| E | the entry loop read `BYTES_OFFSET` through `IMAGE_ENTRY` (0), not `ANNOTATION_ENTRY` (3) | the pointer came back as `(bytesOffset << 24) \| identifier` |
| F | `read_annotations` read nine fields from the block header, not the entry | every entry decoded to identical bytes |
| G | `read_annotations` never inserted what it built | the reader returned an **empty map** for any file with annotations |

A and E are the same confusion the block table exists to prevent: associated
images and annotations are unrelated blocks, and an annotation merely *carrying*
a PNG or SVG payload does not make it one.

**Coverage now in place**, so a fixture only has to contain annotations rather
than work around the encoder:

* `ife_v1_oracle_tests` — v1 writes two annotations, the generated layer reads
  them back, all nine fields plus both payloads. They differ in every field
  deliberately: one entry read through the block header decodes correctly by
  accident, and only a differing sibling exposes it.
* `ife_v1_fixture` — four annotations, one per `annotation_types` value, so
  every format the specification defines appears in a slide the shipped encoder
  wrote.
* `ife_example_parity` — the example prints annotations, so v1's
  `abstract_file_structure` and the runtime's are compared on the annotation
  path and must agree byte for byte. Iterated through
  `Metadata::annotations`, a `std::set`, because an `unordered_map`'s order is
  not a property two implementations must share.

## Still blocked: annotation groups

`ANNOTATION_GROUP_SIZES` and `ANNOTATION_GROUP_BYTES` remain uncovered, and for
a different reason than the above: v1 can read and validate groups but has no
`STORE_` for either, and `AnnotationArrayCreateInfo` carries no group fields.
That is a **feature the shipped encoder never had**, not a defect in it, so
`STORE_ANNOTATION_ARRAY` now writes `NULL_OFFSET` to both — the honest value
for "no groups" — rather than leaving the buffer's contents in place.

Covering them means writing group support into a layer that is being retired.
Deferred deliberately; if it stays deferred, record both blocks as covered by
the generated writers alone and say so, rather than leaving them looking
pending.

## Edge cases worth encoding deliberately

These are the cases where a reader can be wrong at the right offset, which is
the class no self-consistent test reaches.

* **`u40` tile offsets above 4 GB — covered, and no longer a fixture
  requirement.** `ife_large_file_tests` (`130bb97`) validates a whole slide
  over 4 GiB, written by v1 and read through the generated layer. The file is
  sparse: 4.00 GiB long, 28 KiB actually allocated, so it runs in CI rather
  than needing a hosted download. **No hosted fixture needs to exceed 4 GB**,
  which removes the one requirement that would have made the corpus expensive.
  POSIX and 64-bit only — NTFS needs `FSCTL_SET_SPARSE` first, or the same
  test writes 4 GB for real.
* **`NULL_TILE` slots.** Both published files are fully dense. A sparse layer
  is the only thing that exercises the sentinel.
* **A non-zero `FILE_REVISION`**, so the field is not confirmed only at 0.
* **Both byte orders of the same fixture** are unnecessary — IFE is
  little-endian at every version, and the s390x job already covers the reader.
* **1.1 features**: `Z_PLANES` > 1, a non-256 `TILE_SIZE`, `MICRONS_PLANE`, and
  a tile frame carrying the optional 11-byte trailer.

## Delivery

**Host the files; commit only a manifest.** `iris.exampleslides.org`
(Cloudflare R2, registered 2026-08-12) is the host. The two snapshot files
from MIGRATION 6.3/6.4 (`v1_snapshot.test_slide`, `v1_tile_offsets_full_width.bin`)
are the first fixtures on it, fetched through this same manifest, so the
oracle and the corpus share one path. Any static host works — with fixtures
this small, HTTP range support is **not** required, which is what makes the
small-fixture decision pay off twice.

`tests/corpus/manifest.json` (committed, text) records for each fixture: URL,
byte size, SHA-256, IFE version, and the block types it covers. CMake fetches
into the gitignored `.deps/corpus/` and verifies the digest before any test
runs. The digest is what makes a hosted corpus reproducible; without it the
gate silently changes meaning when a file is re-uploaded.

The corpus is a **living set**, not a one-shot: fixtures are added as coverage
grows (the five deferred blocks) and as new IFE versions land, each recorded
in the manifest with the version it proves — the digest is what makes those
re-uploads safe.

**The corpus test must fail loudly when the corpus is unreachable, not skip.**
A gate that disables itself on a network error is the `ctest`-from-stale-binaries
failure mode wearing a different hat. Provide `-DIFE_CORPUS=OFF` as the
explicit opt-out, and cache by manifest digest in CI so the normal path does
not touch the host at all. The 6.3/6.4 snapshot fetch is **not** behind that
opt-out: it is unconditional, like the IrisHeaders `FetchContent`, because the
oracle (MIGRATION 6.5) depends on it — `-DIFE_CORPUS=OFF` disables only the
corpus test target.

## Harness

`tests/ife_corpus_tests.cpp`, one target, iterating the manifest:

1. `IFE::Window::resident` over the fetched file — these are small enough to map
   whole, so no ranged transport is needed.
2. `validate_file_structure` must succeed.
3. Walk every `points_to` edge and confirm each block's recovery tag and
   self-validating `VALIDATION` word.
4. Assert the manifest's declared coverage was actually observed — a fixture
   that stops containing annotations must fail, not quietly narrow the gate.
5. Record which of the 18 block types the corpus reached, and fail if a block
   the manifest claims is absent.

Step 4 is the one that keeps this honest: without it the corpus degrades
silently, exactly as `ANNOTATION_JPEG` did.

## Where oracle coverage stands

Counted from what v1 actually writes across `ife_v1_oracle_tests` and
`ife_v1_fixture`: **13 of the 18 block types**, each written by the shipped
encoder and read back through the generated layer.

Covered: `FILE_HEADER`, `TILE_TABLE`, `LAYER_EXTENTS`, `TILE_OFFSETS`,
`METADATA`, `ATTRIBUTES`, `ATTRIBUTE_SIZES`, `ATTRIBUTE_BYTES`, `IMAGES`,
`IMAGE_BYTES`, `ICC_PROFILE`, `ANNOTATIONS`, `ANNOTATION_BYTES`.
(Re-verified 2026-08-12: 13 distinct `STORE_*` calls in `ife_v1_fixture.cpp`,
2,857 bytes, digest recorded in MIGRATION 6.3.)

The remaining five, each with a reason rather than a gap — together they are
exactly 10 unexecuted generated functions: `CLINICAL_METADATA`'s
`validate_deep(VisitPath&)`, `validate`, `entries_begin`, `encoding`, `bytes`;
framed `TILE_PIXEL_DATA`'s `validate_deep(VisitPath&)`; `store` and `size_of`
for both; `CIPHER` and the two annotation-group blocks contribute only their
no-arg `validate_deep` (their other functions run in-memory without a
fixture). See MIGRATION §Phase 6 execution plan.

| Block | Why not |
|---|---|
| `CIPHER` | deferred; the encoder may never write one |
| `ANNOTATION_GROUP_SIZES`, `ANNOTATION_GROUP_BYTES` | v1 has no group writer |
| `CLINICAL_METADATA` | 1.1 — v1 writes 1.0 and cannot produce it |
| `TILE_PIXEL_DATA` | the framed form is 1.1, same reason |

**This changes what the corpus is still for.** Its original argument was that
twelve block types had no real-encoder bytes behind them; eleven of those now
do. What hand-built fixtures cannot supply is a file produced by the *whole*
Iris-Codec pipeline from real scanner output — realistic pyramids, real
attribute vocabularies, values nobody thought to write by hand — and the last
three blocks above, which need encoder features that do not exist yet. That is
a narrower and more honest case for the corpus than the one this document
opened with, and it is still worth making.

## Exit

Every block type reached by at least one fixture — full real-byte coverage.
Deleting v1 is **not** blocked on this: the digest-pinned snapshot (MIGRATION
6.3) preserves the oracle, and the cutover plan (MIGRATION §Phase 6 execution
plan) is written to complete without the corpus. This document is the path to
covering the five blocks no snapshot contains, which is the only way the
coverage the oracle proved does not quietly narrow.
