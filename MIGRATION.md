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
| 1 | Lock-free VMA: `IFE_Memory`, `claim_space`, `StreamHead`, `Memory::View` | ✅ landed (PR #1) |
| 2 | `IFE_FieldKind`, partitioned `RECOVERY_TAG`, `0x8000` array bit, `TypeTraits<T>` | ✅ landed (this PR) |
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

## Phase 2 — what landed

* `src/IFE_Types.hpp` — header-only type system in a new `IFE` namespace.
  * `enum class IFE_FieldKind { SCALAR, ARRAY, BLOCK, CHOICE, STRING }` —
    the tag carried by every `Reflective::Entry` in Phase 6.
  * Partition constants: `RECOVERY_PARTITION_PRIMITIVE` (`0x01xx`),
    `RECOVERY_PARTITION_DATATYPE` (`0x02xx`),
    `RECOVERY_PARTITION_RESOURCE` (`0x03xx`), and the array bit
    `RECOVERY_ARRAY_BIT = 0x8000` with the `IsArrayTag(uint16_t)` predicate.
  * `enum class RECOVERY_TAG : uint16_t` covering every legacy
    `IrisCodec::Serialization` resource (HEADER, TILE_TABLE, METADATA,
    ATTRIBUTES, LAYER_EXTENTS, TILE_OFFSETS, ATTRIBUTES_SIZES,
    ATTRIBUTES_BYTES, ASSOCIATED_IMAGES, ASSOCIATED_IMAGE_BYTES,
    ICC_PROFILE, ANNOTATIONS, ANNOTATION_BYTES, ANNOTATION_GROUP_SIZES,
    ANNOTATION_GROUP_BYTES, CIPHER, UNDEFINED) plus primitive scalars
    (`uint8/16/32/64`, `int8/16/32/64`, `float`, `double`) and the
    `LAYER_EXTENT` BLOCK datatype. Every ARRAY variant is pre-OR'd with
    `0x8000`.
  * `static_assert`s pin every tag to its expected partition and numeric
    value, so a future schema edit cannot silently re-number a tag.
  * `template<class T> struct TypeTraits` specializations for the primitives
    the codebase actually serializes today, plus `LayerExtentBlock` as the
    first BLOCK datatype. Wire sizes are pinned against the legacy
    `IrisCodec::TYPE_SIZES` and `LAYER_EXTENT::SIZE` so they cannot drift
    from the on-disk encoder.
  * `constexpr legacy_to_tag(IrisCodec::RECOVERY)` /
    `tag_to_legacy(RECOVERY_TAG)` bridge the v1 wire format and the new
    partitioned tag space. Every legacy resource is round-trip closed by
    `static_assert`.
* `tests/ife_types_tests.cpp` — table-driven runtime tests for the
  legacy<->tag mapping, partition helpers, and `TypeTraits` specializations.
  Most invariants are already pinned at compile time inside `IFE_Types.hpp`.

### Soft-deprecation deviation

The migration plan called for marking `enum IrisCodec::RECOVERY` itself
`[[deprecated]]`. Doing so would emit a warning at every existing
`RECOVERY recovery = RECOVER_*;` field declaration in
`IrisCodecExtension.hpp`, polluting non-substrate downstream builds (notably
the Iris-Codec Community Module). PR #1 explicitly preserved binary parity
for those builds. The deprecation is therefore documented in `IFE_Types.hpp`
and here only; a hard `[[deprecated]]` is staged for Phase 6 when the flag
default flips and legacy callers are already migrating.

### Dormant by design

Phase 1 introduces **no on-disk format change** and **no public ABI change**.
The new headers are only compiled when `IFE_USE_FASTFHIR_SUBSTRATE=ON`, and
even then they are not yet wired into `Abstraction::File` or
`validate_file_structure`. Wiring happens in Phase 4. Phase 2 likewise adds
only header-only metadata — `IFE_Types.hpp` is exported alongside
`IFE_Memory.hpp` when the substrate flag is ON, and both legacy
`IrisCodec::RECOVERY` / `IrisCodec::Serialization` block layouts continue to
define the v1 wire format until Phase 4.

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
