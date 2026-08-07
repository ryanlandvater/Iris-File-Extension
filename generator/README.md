# `generator/` — the IFE code generator

Phase 3 deliverable (see `MIGRATION.md`). Turns the committed spec JSON
(`spec/ife_fields.json`, `spec/ife_constants.json`) into the C++ that
Iris-File-Extension compiles — and the derived layout tables the
LaTeX/HTML specification document renders.

Run it with:

```bash
python3 -m generator                    # writes generated_source/ + generated_docs/
python3 -m generator --out-dir DIR      # write the C++ elsewhere
python3 -m generator --check            # CI: exit 1 if outputs drifted
```

## Output contract

| Output | Contract |
|--------|----------|
| `spec/*.json` | **Source of truth. Committed.** Fields, types, values. |
| `generated_source/` | **Freely regenerable. Gitignored.** C++ layer: `IFE_Constants.hpp`, `IFE_VTables.hpp`. Rebuilt whenever missing at CMake configure, or on demand with `-DIFE_RUN_GENERATOR=ON`. |
| `generated_docs/` | **Freely regenerable. Gitignored.** Doc tables: `layout_tables.md`. |

Nothing generated is permanent — but the *vtable layout* is, which is why
the JSON is versioned append-only.

## CLI contract (final)

| Flag | Default | Meaning |
|------|---------|---------|
| `--schema-dir` | `spec/` | Directory holding `ife_fields.json` + `ife_constants.json` |
| `--out-dir` | `generated_source/` | Where generated C++ is written |
| `--docs-dir` | `generated_docs/` | Where generated documentation is written |
| `--check` | — | Regenerate in memory and fail (exit 1) if outputs drifted |

CMakeLists.txt invokes exactly this interface at configure time; changing
flags requires updating CMakeLists.txt in the same change.

## Module map

| Module | Role |
|--------|------|
| `__main__.py` | CLI entry point (`python -m generator`); contract above. |
| `pipeline.py` | Stage orchestration: load JSON → derive → emit → `--check`. No code generation lives here. |
| `model/layout.py` | Layout derivation — the single implementation of the offset/size rules; offsets are never read from the JSON. |
| `emit/cpp.py` | C++ emission: `IFE_Constants.hpp` (enums + sentinels), `IFE_VTables.hpp` (vtables). |
| `emit/docs.py` | Markdown emission: derived layout tables (first cut of the doc emitter; full spec assembly is Phase 5). |

CMake's configure-time gate (`EXISTS generator/pipeline.py`) runs
`python -m generator` whenever `generated_source/IFE_VTables.hpp` is
missing or `-DIFE_RUN_GENERATOR=ON`.

## Contract

* Output must be byte-stable (stable ordering, no timestamps) so CI can
  diff-check regeneration with `--check`.
* Never hand-edit `generated_source/` or `generated_docs/`; fix the JSON
  and regenerate.
