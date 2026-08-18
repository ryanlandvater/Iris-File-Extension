# `generator/` — the IFE code generator

Turns the committed spec JSON
(`spec/ife_header.json`, `spec/ife_fields.json`, `spec/ife_constants.json`)
into the C++ that Iris-File-Extension compiles — and the derived layout tables
the LaTeX/HTML specification document renders.

Run it with:

```bash
python3 -m generator                    # writes generated_source/ + generated_docs/
python3 -m generator --out-dir DIR      # write the C++ elsewhere
python3 -m generator --validate         # conflicts, dangling references, wire-witness drift
python3 -m generator --check            # CI: exit 1 on stale witness or banner-hash parity break
```

`--validate` runs automatically before every generation, so a broken spec
never reaches the emitters. It also recomputes the wire witness and fails on
drift against the committed baseline in `tests/wire/witness.json` — the
append-only gate (see below) — and prints a non-fatal note when the spec has
grown facts the baseline has not recorded yet. `--check` turns that note into
a failure: the committed evidence must be current for the drift gate to mean
anything.

## Output contract

| Output | Contract |
|--------|----------|
| `spec/*.json` | **Source of truth. Committed.** Identity, fields, types, values. |
| `tests/wire/witness.json` | **Append-only evidence. Committed.** The wire witness — every fact that reaches the stream, never C++ text. `--validate` fails if a shipped fact changed or disappeared; additions pass silently. Refresh only after a reviewed wire change: see `tests/wire/README.md`. |
| `generated_source/` | **Freely regenerable. Gitignored.** C++ layer: `IFE_Constants.hpp`, `IFE_VTables.hpp`, `IFE_Blocks.hpp` + `IFE_Blocks.cpp`. Rebuilt whenever missing at CMake configure, or on demand with `-DIFE_RUN_GENERATOR=ON`. |
| `generated_docs/` | **Freely regenerable. Gitignored.** Doc tables: `layout_tables.md`. |

The five outputs are registered in one place — `pipeline.py::_render` — and
`--check` verifies whatever that map contains, so adding an artifact never
means remembering to extend the drift gate.

`--check` is **parity, not byte-equality** (XP-2): every generated file's
banner carries the wire-witness sha256, and `--check` compares that against a
fresh computation. A comment edit in an emitter — a version bump, a copyright
year — no longer flags as drift; only a file produced from a different spec
does. Content beyond the banner is unwitnessed by design: the build and the
corpus oracle own that ground. (`attributes.adoc` is the one banner-less
file: it is the AsciiDoc document header, which cannot carry a comment
block.)

Nothing generated is permanent — but the *vtable layout* is, which is why
the JSON is versioned append-only.

## CLI contract (final)

| Flag | Default | Meaning |
|------|---------|---------|
| `--schema-dir` | `spec/` | Directory holding `ife_header.json` + `ife_fields.json` + `ife_constants.json` |
| `--out-dir` | `generated_source/` | Where generated C++ is written |
| `--docs-dir` | `generated_docs/` | Where generated documentation is written |
| `--validate` | — | Check the documents for conflicts, dangling references, and drift against the committed wire witness (`tests/wire/witness.json`), emit nothing (exit 1 on any). Warns — non-fatally — when the spec has unrecorded additions |
| `--check` | — | Fail (exit 1) if any generated file's banner witness hash mismatches the current spec (parity, not byte-equality) or the committed witness is stale |

CMakeLists.txt invokes exactly this interface at configure time; changing
flags requires updating CMakeLists.txt in the same change.

## Module map

```mermaid
flowchart LR
    subgraph src["spec/ — committed source of truth"]
        H[ife_header.json]
        F[ife_fields.json]
        C[ife_constants.json]
    end
    H & F & C --> M[__main__.py<br/><i>argument parsing only</i>]
    M --> P[pipeline.py<br/><i>all file I/O</i>]
    P --> V[validate.py<br/><i>pure predicate</i>]
    V --> W[witness.py<br/><i>wire facts only</i>]
    P -->|loads| B[tests/wire/witness.json<br/><i>committed baseline</i>]
    P --> L[model/layout.py<br/><i>numbers only</i>]
    L --> CPP[emit/cpp.py<br/><i>text only</i>]
    L --> DOC[emit/docs.py<br/><i>text only</i>]
    CPP --> O1[IFE_Constants.hpp<br/>IFE_VTables.hpp<br/>IFE_Blocks.hpp + .cpp]
    DOC --> O2[layout_tables.md]
```

| Module | Role |
|--------|------|
| `__main__.py` | CLI entry point (`python -m generator`); contract above. |
| `pipeline.py` | Stage orchestration: load JSON → validate → derive → emit → write or `--check`. The only module that touches the filesystem; no code generation lives here. |
| `validate.py` | Consistency checks across the documents, including the append-only gate: the current tree's witness against the committed baseline. Returns (problems, warnings); raises nothing; reads no files. Warnings are a stale witness baseline — `--check` promotes them to a failure. |
| `witness.py` | The wire witness: captures only what reaches the stream (block tags, per-version field tuples, enum values, named constants), derived through `model/layout.py`. Reads no files. |
| `model/layout.py` | Layout derivation — the single implementation of the offset/size rules; offsets are never read from the JSON. |
| `emit/cpp.py` | C++ emission: `IFE_Constants.hpp` (enums + sentinels), `IFE_VTables.hpp` (vtables), `IFE_Blocks.hpp`/`.cpp` (handles, readers, validators). |
| `emit/docs.py` | Markdown emission: derived layout tables (first cut of the doc emitter; full spec assembly comes with the document pipeline). |

The boundary in that diagram is load-bearing: `model/` never produces text,
`emit/` never does arithmetic on a byte offset, `validate.py` never touches the
filesystem. Arithmetic found in `emit/` is in the wrong file. The witness
baseline is read by `pipeline.py` — the only module that touches the
filesystem — and handed to `validate.py` as data, so the boundary holds.

CMake's configure-time gate (`EXISTS generator/pipeline.py`) runs
`python -m generator` whenever `generated_source/IFE_VTables.hpp` is
missing or `-DIFE_RUN_GENERATOR=ON`.

## Contract

* Output must be byte-stable (stable ordering, no timestamps): the same
  layout always yields the same bytes, which is what makes regeneration
  deterministic and the banner witness hash meaningful. `--check` verifies
  parity against that hash, not character equality. The determinism is
  tested, not assumed — `tests/generator/test_determinism.py` runs the
  generator twice into separate trees and compares (ctest
  `ife_generator_determinism`; CI adds `PYTHONHASHSEED=0/1`, where dict/set
  iteration order is where non-determinism would enter).
* Never hand-edit `generated_source/` or `generated_docs/`; fix the JSON
  and regenerate.
