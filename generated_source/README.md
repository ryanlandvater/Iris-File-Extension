# `generated_source/` — generated C++ for the Iris File Extension

This directory holds the **regenerable** output of the IFE code generator
(`generator/`, Phase 3). Everything here is **gitignored** and freely
regenerated from the committed JSON specification — never hand-edited.

| Path | Contract |
|------|----------|
| `spec/ife_fields.json`, `spec/ife_constants.json` | **Source of truth. Committed.** Every field, type, and value. |
| `generator/` (Phase 3) | Stdlib-only Python package, `python -m generator`. |
| `generated_source/` (this dir) | **Freely regenerable. Gitignored.** vtables, enums, PODs, field keys, validation tables. |

## Regenerate

```bash
python3 -m generator --out-dir generated_source
```

or from CMake:

```bash
cmake -B build -DIFE_RUN_GENERATOR=ON
```

Generation also runs automatically at configure when the anchor file
(`IFE_VTables.hpp`) is missing from this directory. `README.md` is the only
committed file here.

## Why not commit?

The committed JSON is the single source of truth; committing generated
output would create a second, drift-prone copy. Downstream consumers
(FetchContent) receive the generated code at configure time — the generator
is stdlib-only Python, so no toolchain beyond Python 3 is required.

## Contract

* Generator output must be byte-stable (stable ordering, no timestamps).
* Never hand-edit anything here; fix the JSON and regenerate.
* The vtable layouts emitted here are wire constants — see
  `spec/README.md` for the append-only versioning rules.
