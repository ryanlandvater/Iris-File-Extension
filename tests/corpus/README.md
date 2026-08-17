# Conformance corpus

Real bytes on disk, pinned by digest. The corpus exists so the test suite
proves the stack reads **files that exist**, rather than re-deriving the format
from the same schema it was generated from.

> **`v1_snapshot.test_slide` is no longer an independent witness — read this
> before relying on it.** It was written by the shipped v1 encoder, which made
> it evidence about *another implementation's* bytes: the one check that cannot
> be fooled by a reader and a writer agreeing with each other about the wrong
> thing. That ended with the 1.0 correction that gave `ATTRIBUTE_SIZE` its
> `KIND` byte. The entry went from six bytes to seven, a stride of six stopped
> being conformant, and the array validator now rejects the original (2857 B,
> `658ead7c…`) — correctly. v1's writers were deleted in Phase 6, so nothing
> but the generated layer can produce a corrected file.
>
> The replacement is written by `tests/ife_snapshot_writer.cpp` against a
> **1.0-only** generated layer, derived from the committed spec by
> `tests/fixtures/build_baseline_spec.py`. That indirection is load-bearing:
> `store()` always lays out the newest version it knows, so a writer built from
> the committed spec emits 1.1, and the oracle's substance is a 1.1 reader over
> 1.0 bytes — `TILE_LENGTH` and `Z_PLANES` reading back absent, the layer
> extent stride at the 1.0 entry size. A 1.1 fixture proves none of that.
>
> The result is 2858 B — one byte larger than v1's, and that byte is its single
> attribute's `KIND`. Every other block sits at the offset v1 put it at,
> `ATTRIBUTE_SIZES` included, at byte 907 in both.
>
> **What is kept:** the digest pin, so a schema edit that moves a shipped field
> breaks reading a file nobody regenerated. **What is lost:** the cross-check,
> until a second implementation exists to write a fixture. One assertion died
> outright rather than moving — v1 stored `METADATA_FREE_TEXT` as an alias of
> `METADATA_I2S`, and demonstrating that errata needs bytes a v1 encoder
> actually wrote.

Files are **hosted, not committed**: `iris.exampleslides.org` (Cloudflare R2)
serves them, `manifest.json` pins each by SHA-256, and CMake fetches into the
gitignored `.deps/corpus/` at configure time. The digest is what makes a hosted
corpus reproducible.

## `manifest.json`

Per fixture: `url`, `size`, `sha256`, the IFE `version` it was written under,
and the `blocks` it covers. The coverage list is not documentation — the
harness asserts it was actually observed, so a fixture that stops containing
annotations fails loudly instead of quietly narrowing the gate.

Adding a fixture is a manifest entry plus an upload. The corpus is a living
set: it grows with coverage and with each new IFE version.

## Coverage

The two live fixtures reach **13 of the 18 block types**. Uncovered, each
because no encoder can currently write it:

| Block | Blocked on |
|---|---|
| `CIPHER` | no encryption in the encoder; may stay permanently uncovered |
| `ANNOTATION_GROUP_SIZES`, `ANNOTATION_GROUP_BYTES` | no group writer |
| framed `TILE_PIXEL_DATA` | 1.1 frame not yet written by the encoder |
| `CLINICAL_METADATA` | 1.1 block not yet written by the encoder |

Worth encoding deliberately when fixtures are next produced: `NULL_TILE` slots
(both published files are dense, so the sentinel is never exercised), a
non-zero `FILE_REVISION`, tile encodings other than JPEG, and the 1.1 features
— `Z_PLANES` > 1, a non-256 `TILE_LENGTH`, `MICRONS_PLANE`, and a tile frame.

Two things need no fixture. Slides over 4 GiB are covered by
`ife_large_file_tests` (a 4.00 GiB sparse file, 28 KiB actually allocated), and
both byte orders are unnecessary — IFE is little-endian at every version, and
the s390x CI leg exercises the reader.

## Rules that are easy to get wrong

* **Unreachable corpus fails; it does not skip.** `-DIFE_CORPUS=OFF` is the
  explicit opt-out, and it disables only the corpus test — never the snapshot
  fetch, which the oracle depends on.
* **The zone's bot protection rejects `Python-urllib`** (Cloudflare error
  1010). `tools/fetch_corpus.py` sends `ife-corpus-fetch/1.0`. Keep a
  non-default user agent if the host ever changes.
* **The published [Iris-Example-Files](https://github.com/IrisDigitalPathology/Iris-Example-Files)
  do not serve as a corpus.** Both are the same specimen with empty metadata
  and reach 6 of 18 block types; building on them would narrow real-byte
  coverage rather than replace it. Fixtures are purpose-built and small — a
  valid pyramid with real JPEG tiles is under 5 MB.
