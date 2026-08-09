# `generated_source/` — generated C++ for the Iris File Extension

This directory holds the **regenerable** output of the IFE code generator
(`generator/`, Phase 3). Everything here is **gitignored** and freely
regenerated from the committed JSON specification — never hand-edited.

| Path | Contract |
|------|----------|
| `spec/ife_header.json`, `spec/ife_fields.json`, `spec/ife_constants.json` | **Source of truth. Committed.** Every field, type, and value. |
| `generator/` (Phase 3) | Stdlib-only Python package, `python -m generator`. |
| `generated_source/` (this dir) | **Freely regenerable. Gitignored.** The four files below. |

| File | Content |
|------|---------|
| `IFE_Constants.hpp` | Enumerations and statically defined values. Dependency-free. |
| `IFE_VTables.hpp` | Derived byte offsets and cumulative sizes, per block and per array entry, per version group. Dependency-free. |
| `IFE_Blocks.hpp` | Typed block handles: accessors, `points_to` navigation, structural validators. Declarations only; includes the Iris headers itself. |
| `IFE_Blocks.cpp` | Their definitions. Compiled into the library, or folded into the header by `#define IFE_HEADER_ONLY`. |

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

## Consuming this layer

**`IFE_HEADER_ONLY` is the only way to reach it** (decision 4.0-D). Nothing
here is exported from the shared library — `IFE::blocks`, `IFE::vtables` and
`IFE::constants` carry no export marking, and a test fails the build if any of
them becomes visible:

```cpp
#define IFE_HEADER_ONLY
#include "IFE_Blocks.hpp"       // definitions fold in; nothing to link
```

or compile `generated_source/IFE_Blocks.cpp` into your own target.

That is deliberate, not an oversight. This layer is field arithmetic — inlining
a `u24` load beats calling it across a library boundary — and it is *generated*,
so it must stay free to change whenever the schema does. An exported symbol is
one that cannot. The stable, exported API is the semantic layer:
`IrisCodec::validate_file_structure`, `abstract_file_structure`,
`generate_file_map`, `recover_file_structure`, and the `Abstraction::` structs.

## Contract

* Generator output must be byte-stable (stable ordering, no timestamps).
* Never hand-edit anything here; fix the JSON and regenerate.
* The vtable layouts emitted here are wire constants — see
  `spec/README.md` for the append-only versioning rules.
