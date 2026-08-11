# Phase 6 — Conformance corpus

Refinement pass for Phase 6 item 2. The corpus exists to replace what
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
| Tile offsets | 183,146 entries, **0** `NULL_TILE` slots |
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
| `ANNOTATIONS`, `ANNOTATION_BYTES` ⚠ | ≥1 of each `annotation_types` value — `ANNOTATION_PNG`, `JPEG`, `SVG`, `TEXT` |
| `ANNOTATION_GROUP_SIZES`, `ANNOTATION_GROUP_BYTES` ⚠ | ≥1 named annotation group |
| `CIPHER` ⚠ | one encrypted fixture, if the encoder can write one; otherwise record it as permanently uncovered |
| `CLINICAL_METADATA` ⚠ | 1.1 only — one fixture per `clinical_encodings` value the encoder supports |

## Edge cases worth encoding deliberately

These are the cases where a reader can be wrong at the right offset, which is
the class no self-consistent test reaches.

* **`u40` tile offsets above 4 GB.** Every published file is under 2 GB, so the
  top byte of every `TILE_OFFSETS` entry is zero and a `u40` read as `u32`
  would pass. One fixture must exceed 4 GB, or the case stays covered only by
  `test_v1_packed_widths_at_full_width` and dies with v1.
* **`NULL_TILE` slots.** Both published files are fully dense. A sparse layer
  is the only thing that exercises the sentinel.
* **A non-zero `FILE_REVISION`**, so the field is not confirmed only at 0.
* **Both byte orders of the same fixture** are unnecessary — IFE is
  little-endian at every version, and the s390x job already covers the reader.
* **1.1 features**: `Z_PLANES` > 1, a non-256 `TILE_SIZE`, `MICRONS_PLANE`, and
  a tile frame carrying the optional 11-byte trailer.

## Delivery

**Host the files; commit only a manifest.** `irisdigitalpathology.org` is
registered but currently resolves nowhere, so `examples.` is new
infrastructure. Any static host works — with fixtures this small, HTTP range
support is **not** required, which is what makes the small-fixture decision pay
off twice.

`tests/corpus/manifest.json` (committed, text) records for each fixture: URL,
byte size, SHA-256, IFE version, and the block types it covers. CMake fetches
into the gitignored `.deps/corpus/` and verifies the digest before any test
runs. The digest is what makes a hosted corpus reproducible; without it the
gate silently changes meaning when a file is re-uploaded.

**The corpus test must fail loudly when the corpus is unreachable, not skip.**
A gate that disables itself on a network error is the `ctest`-from-stale-binaries
failure mode wearing a different hat. Provide `-DIFE_CORPUS=OFF` as the
explicit opt-out, and cache by manifest digest in CI so the normal path does not
touch the host at all.

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

## Exit

Every block type reached by at least one fixture; `ife_v1_oracle_tests` and
`src/IrisCodecExtension.*` removable without loss of real-byte coverage. Until
then item 3 stays blocked, and that is the correct state — not a delay to work
around.
