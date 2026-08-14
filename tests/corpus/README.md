# Conformance corpus

Real bytes, written by a shipped encoder. The corpus exists so the test suite
proves the stack reads **files that exist**, rather than agreeing with a second
description of the format — which is all any fixture built by our own writers
can prove.

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
