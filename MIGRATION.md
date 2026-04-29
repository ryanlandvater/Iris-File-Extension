# IFE 2.x Migration — FastFHIR Substrate

> **Status:** in progress (Phase 1 of 6 landed). Targeted version: `2.0.0-alpha`.

The Iris File Extension is migrating from the eager-mapping `Abstraction::File`
pattern to a lock-free Virtual Memory Arena (VMA) substrate ported from
FastFHIR. Goals:

* Zero-copy reads via reflective lenses (`Reflective::Node` / `Entry`).
* Lock-free concurrent block ingestion (`claim_space` via `fetch_add`).
* Schema-driven codegen (`ifc.py`) replacing hand-written byte-offset enums.
* A universal 10-byte `DATA_BLOCK` header for all blocks on disk.

The migration is staged across **six sequential PRs**. Each PR is independently
reviewable, buildable, and testable. The legacy `IrisCodecExtension.{hpp,cpp}`
API continues to build and behave unchanged until **Phase 6**.

## Feature flag

A new CMake option `IFE_USE_FASTFHIR_SUBSTRATE` (default **OFF**) gates the
new substrate. While OFF:
- The build is byte-identical to the pre-migration build.
- No new headers are exported.
- No on-disk format change.

Downstream consumers (notably the Iris-Codec Community Module) are not
affected until they explicitly opt in.

## Phase status

| Phase | Scope | Status |
|------:|-------|--------|
| 1 | Lock-free VMA: `IFE_Memory`, `claim_space`, `StreamHead`, `Memory::View` | ✅ landed (this PR) |
| 2 | `IFE_FieldKind`, partitioned `RECOVERY_TAG`, `0x8000` array bit, `TypeTraits<T>` | pending |
| 3 | `ifc.py` JSON-schema-driven codegen → `IFE_DataTypes.hpp`, `IFE_VTables.hpp`, `IFE_FieldKeys.hpp`, WASM stubs | pending |
| 4 | Universal 10-byte `DATA_BLOCK` header, `Builder::amend_pointer` (**v2 on-disk break**) | pending |
| 5 | `IFE_ARRAY` block, `KIND_AND_STEP`, removal of `std::vector` from read paths | pending |
| 6 | `IFE_Builder`, `Reflective::Node`/`Entry`, deprecate `Abstraction::File`, flag default ON | pending |

## Phase 1 — what landed

* `src/IFE_Memory.hpp` / `src/IFE_Memory.cpp` — Handle/Body lock-free arena.
  * 64-bit atomic write-head at bytes [0,8); bytes [8,16) reserved for the
    Phase 4 `FILE_HEADER` migration.
  * `claim_space(bytes)` uses a single `fetch_add` on the low 63 bits.
  * `STREAM_LOCK_BIT = 1ULL << 63`, acquired via the RAII `StreamHead` guard
    using a CAS that does not disturb the offset bits. Concurrent writers
    cooperatively spin until the lock clears.
  * `Memory::View` holds a `std::shared_ptr<const Body>`, pinning the arena
    across asynchronous network/disk I/O even after every `Memory` handle
    has been dropped.
* `tests/ife_memory_tests.cpp` — self-contained unit + stress tests
  (multi-thread `claim_space`, stream-lock exclusion, view lifetime,
  exhaustion, error paths). TSan-clean with `-fsanitize=thread`.

### Dormant by design

Phase 1 introduces **no on-disk format change** and **no public ABI change**.
The new headers are only compiled when `IFE_USE_FASTFHIR_SUBSTRATE=ON`, and
even then they are not yet wired into `Abstraction::File` or
`validate_file_structure`. Wiring happens in Phase 4.

## Building with the substrate

```bash
cmake -B build -S . \
      -DIFE_USE_FASTFHIR_SUBSTRATE=ON \
      -DIFE_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For thread-sanitizer validation of the lock-free paths:

```bash
cmake -B build-tsan -S . \
      -DIFE_USE_FASTFHIR_SUBSTRATE=ON \
      -DIFE_BUILD_TESTS=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
cmake --build build-tsan --target ife_memory_tests
./build-tsan/ife_memory_tests
```

## Open items blocking later phases

These items must be resolved before Phases 2-6 can proceed; they are tracked
on the parent migration tracking issue:

1. Pointer to a real FastFHIR repository (`FF_Memory`, `ffc.py`). Phase 1
   inferred semantics from the migration spec; later phases need the source.
2. Confirmation that the v2 on-disk break in Phase 4 is in scope (legacy v1
   files will not be readable by the new substrate; a separate read-only
   compatibility shim is *not* in scope unless explicitly requested).
3. Confirmation of `Abstraction::File` ABI break in Phase 6.
4. Whether `jinja2` is acceptable for `ifc.py` templating (Phase 3).
5. A small corpus of real `.iris` files for round-trip parity tests.
