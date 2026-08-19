# Contributing to Iris-File-Extension

Thanks for contributing. IFE is a binary slide format — the bar for
correctness is high, and a few rules are absolute. Read `README.md`, the
cross-pollination work orders at the top of `MIGRATION.md`, and this page
before opening a PR.

## The absolutes

1. **Append-only is the wire contract.** Never move, resize, retype or remove
   a field that already exists in `spec/*.json`. New facts arrive only by
   *addition* — a new version group, a new field at the end of a group, a new
   block with a new recovery tag — and retirement happens only via
   `deprecated`. `python3 -m generator --validate` enforces this against the
   committed witness; a PR that breaks it will be rejected regardless of its
   other merits.
2. **Never hand-edit generated output.** `generated_source/` and
   `generated_docs/` carry a DO-NOT-EDIT banner and are overwritten at
   configure time. Fix the emitter in `generator/emit/` and regenerate.
3. **`tests/wire/witness.json` is evidence, not output.** It is committed on
   purpose — the gate compares against it — and is refreshed only as part of a
   reviewed wire change (see "What a wire change requires" below). A witness
   update without a corresponding spec change is a red flag. Its history is
   not unbroken: it records the deliberate 1.0 `ATTRIBUTE_SIZE` correction,
   the one break of the invariant (see `tests/wire/README.md`).
4. **The corpus is pinned, not trusted.** `tests/corpus/manifest.json` pins
   each hosted fixture by SHA-256; a re-uploaded file the manifest has not
   blessed is rejected, not silently adopted. Never touch corpus bytes without
   updating the manifest in the same change.

## The two guards: the contract and the bytes

IFE's compatibility is defended twice, at two different times, and the two
guards are not interchangeable:

| | `tests/wire/witness.json` — the contract | `tests/corpus/*.test_slide` — the bytes |
|---|---|---|
| What it is | Every fact the spec puts on the wire — recovery tags, per-version field (name, type, width, offset), entry sizes, enum values, named constants — derived from `spec/*.json` through `generator.model.layout.derive_layout`, the same derivation the emitters use | Real files on disk, pinned by SHA-256, hosted on Cloudflare R2 |
| When it runs | `python3 -m generator --validate` — before generation, before any C++ compiles, in CI | `ctest` — after a full build |
| Coverage | **All 18 blocks, every version group, every enum member, every constant** | **All 18 block types**, plus the optional tile frame, across one frozen witness per version (1.0 and 1.1) and a `CIPHER` variant — so 1.1 additions are covered by real bytes, not only by the contract |
| What it catches | Any shipped field moved, widened, retyped or removed; any enum or constant value changed | Anything that stops a real file decoding end-to-end — including derivation bugs the witness shares with the generator |
| What it permits | Additions, silently — that is what append-only means | — |
| Error it gives | Names the field and both offsets, e.g. `CHANGED blocks.TILE_TABLE.fields.1.0: 'X_EXTENT' ['u32', 4, 36] → ['u32', 4, 40]` | A failed test; the bisect is yours |
| Refresh | Deliberate, reviewed, committed — exact command in `tests/wire/README.md` | A manifest entry, an upload, and a `data` entry in `tests/tests.bzl` — see `tests/corpus/README.md` |

**Why both.** The witness is complete, precise, and instant, but it is
derived from the same schema and the same `derive_layout` the generator uses —
it can be wrong in exactly the way the code is wrong, and a shared bug passes
both. The pinned corpus bytes are the one check that cannot be fooled by a
reader and a writer agreeing with each other about the wrong thing. The
corpus, in turn, pins bytes rather than the schema, and it checks that a block
is *present and walkable*, not that every function of it behaves. **Contract first,
bytes second**: the witness catches the schema edit at `--validate` time, the
corpus catches what the implementation actually does at `ctest` time — and
neither replaces the other.

## What a wire change requires

1. Edit `spec/*.json` append-only — addition, never mutation.
2. `python3 -m generator --validate` stays green (the gate permits additions).
3. Refresh `tests/wire/witness.json` deliberately, in the same change. A
   baseline that does not record a new field cannot catch that field's later
   mutation — the refresh is what keeps the gate growing with the spec. CI
   enforces this: `--check` fails until the refresh lands (`--validate`
   prints a non-fatal note), so the discipline is mechanical, not just
   documented.
4. Corpus bytes change only when coverage deliberately grows, as a manifest
   entry plus an upload in the same change. A rule-1-compliant edit never
   breaks a pinned file, so it never *requires* a corpus change.
5. **A ratified version's witness is frozen.** Corpus fixtures are one witness
   per IFE version; the newest is mutable while its version is draft and fixed
   when it ratifies. Growing coverage adds a fixture, it does not rewrite a
   shipped one — a rewritten 1.0 witness would delete the only real-bytes proof
   that a current reader still reads 1.0 files.

## Build & test

```bash
python3 -m generator --validate        # spec consistency + append-only gate
python3 -m generator && python3 -m generator --check
cmake . -B build -DIFE_BUILD_TESTS=ON && cmake --build build --config Release -j \
  && ctest --test-dir build -C Release --output-on-failure
```

Traps that have already cost this project time:

- The generator's determinism is tested, not assumed:
  `tests/generator/test_determinism.py` runs it twice into temp trees and
  compares every emitted file (ctest `ife_generator_determinism`; CI adds
  `PYTHONHASHSEED=0/1`, where dict/set iteration order is where
  non-determinism would enter).

- A green `tests` job says nothing about whether the corpus host is up. The
  cache key is the manifest digest, so an unchanged manifest means a cache hit
  and no fetch at all. That is intended — nothing in that run needs the host —
  but do not read it as evidence the fixtures are reachable. To check that
  directly: `python3 tools/fetch_corpus.py --manifest tests/corpus/manifest.json
  --dest "$(mktemp -d)"`, which forces a real fetch and verifies every digest.

- A spec addition without a refreshed `tests/wire/witness.json` makes
  `--check` fail (`--validate` only notes it). Refresh deliberately with
  `python3 tools/refresh_witness.py`, in the same change as the spec edit.
- `-DIFE_BUILD_TESTS=ON` is required; without it ctest reports "No tests were
  found" and that reads as success.
- Chain build and test with `&&` — ctest reports PASS from stale binaries
  after a failed build.
- After changing `spec/*.json` or an emitter, run `cmake .` (configure), not
  just `cmake --build` — fixtures are generated at configure time.
- The corpus is fetched at configure into `.deps/corpus/`, inside the
  `if(IFE_BUILD_TESTS)` guard, and a failed fetch is a `message(FATAL_ERROR)`
  (`tests/tests.cmake`). It breaks the whole configure, not just the corpus
  test — which is why a manifest entry must never name an object that is not
  yet hosted. A build with tests off never contacts the host at all.

## Picking work

Pending work lives in the cross-pollination work orders at the top of
`MIGRATION.md` and in the phase plans below them. Claim one task ID per
session, run the task's *Locate* block before touching anything, and keep the
diff scoped to that task. Unless the order says to commit, leave changes in
the working tree and report.
