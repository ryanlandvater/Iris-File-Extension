# IFE 2.x Migration — FastFHIR Substrate

> **Status:** in progress (Phases 1-3 of 6 landed). Targeted version: `2.0.0-alpha`.

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
| 2 | `IFE_FieldKind`, partitioned `RECOVERY_TAG`, `0x8000` array bit, `TypeTraits<T>` | ✅ landed (PR #2) |
| 3 | `ifc.py` JSON-schema-driven codegen → `IFE_DataTypes.hpp`, `IFE_VTables.hpp`, `IFE_FieldKeys.hpp`, WASM stubs | ✅ landed (this PR) |
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
`validate_file_structure`. Wiring happens in Phase 4. Phases 2-3 likewise add
only header-only metadata (Phase 2) and build-tree-only generated headers
(Phase 3) — `IFE_Types.hpp` is exported alongside `IFE_Memory.hpp` when the
substrate flag is ON, and both legacy `IrisCodec::Serialization::RECOVERY` /
`IrisCodec::Serialization` block layouts continue to define the v1 wire
format until Phase 4.

## Phase 3 — what landed

* `schema/ife_v1.json` — JSON description of the v1 IFE container. Single
  source of truth for every resource (FILE_HEADER, TILE_TABLE, METADATA,
  ATTRIBUTES, LAYER_EXTENTS, TILE_OFFSETS, ATTRIBUTES_SIZES,
  ATTRIBUTES_BYTES, IMAGE_ARRAY, IMAGE_BYTES, ICC_PROFILE, ANNOTATIONS,
  ANNOTATION_BYTES, ANNOTATION_GROUP_SIZES, ANNOTATION_GROUP_BYTES) and
  every sub-block datatype (LAYER_EXTENT, TILE_OFFSET, ATTRIBUTE_SIZE,
  IMAGE_ENTRY, ANNOTATION_ENTRY, ANNOTATION_GROUP_SIZE).
* `tools/ifc.py` — Python 3 stdlib-only codegen (no jinja2 dependency).
  Reads the schema and emits four C++ headers into the build tree.
  Includes a `--check` mode for CI: regenerates to a temp dir and diffs
  against the build-tree files; non-zero exit on drift.
* Generated headers (in `${CMAKE_BINARY_DIR}/generated/IFE/`):
  * `IFE_DataTypes.hpp` — POD reflective structs (`IFE::datatypes::*`)
    for every sub-block datatype, each pinning its on-disk `wire_size`
    via `static_assert`. `sizeof(struct)` is *not* pinned in general
    because narrow on-disk types (uint24 / uint40) widen to natural
    C++ types in memory; it is pinned for `LAYER_EXTENT` because all
    of its fields are 4-byte naturally aligned.
  * `IFE_VTables.hpp` — per-resource `IFE::vtables::<NAME>` namespaces
    with `constexpr` field offsets, field sizes, `header_size`, and a
    `recovery_tag` constant. Replaces the hand-written
    `enum vtable_offsets` / `enum vtable_sizes` blocks in
    `IrisCodecExtension.hpp`.
  * `IFE_FieldKeys.hpp` — `constexpr std::array<FieldKey, N>` per
    resource, where `FieldKey = {name, kind, scalar_tag, offset, size}`.
    Consumed by the Phase 6 reflective reader.
  * `IFE_FieldKeys_wasm.hpp` — `__EMSCRIPTEN__`-gated `extern "C"`
    surface. Stubs only; Phase 6 wires implementations.
* CMake integration (`IFE_USE_FASTFHIR_SUBSTRATE=ON`):
  * Requires Python 3 (`find_package(Python3 REQUIRED)`).
  * Runs `ifc.py` at configure time; `CMAKE_CONFIGURE_DEPENDS` re-runs
    it if either the schema or the codegen script changes.
  * Adds the build-tree generated dir to the include path.
* `tests/ife_codegen_tests.cpp` — parity safety net. For every resource,
  every legacy `IrisCodec::Serialization::<RES>::FIELD` offset, every
  `..._S` size, every `HEADER_SIZE`, and every `recovery` tag is
  cross-checked at compile time against the generated equivalent via
  `static_assert`. A second ctest entry (`ife_codegen_check`) re-runs
  `ifc.py --check` so a stale build directory also fails CI.
* Schema version (`IFE::IFE_SCHEMA_VERSION_MAJOR/MINOR`) is embedded as
  `constexpr` so a translation unit compiled against an older schema
  fails the build against a newer one.

### Build-time dependency

When `IFE_USE_FASTFHIR_SUBSTRATE=ON`, **Python 3 is required at configure
time**. Substrate-OFF builds remain Python-free.

### Codegen contract

- The schema (`schema/ife_v1.json`) is the single source of truth.
- Generated headers live under `${CMAKE_BINARY_DIR}/generated/IFE/` and
  are NEVER checked in — they are regenerated at every CMake configure.
- The legacy `IrisCodec::Serialization` enums in
  `IrisCodecExtension.hpp` are the parity baseline for v1. Phase 4
  flips the relationship: the schema becomes authoritative and the
  legacy enums get removed.

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
