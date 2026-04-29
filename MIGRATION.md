# IFE 2.x Migration — FastFHIR Substrate

> **Status:** in progress (Phases 1-5 of 6 landed). Targeted version: `2.0.0-alpha`.

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
| 3 | `ifc.py` JSON-schema-driven codegen → `IFE_DataTypes.hpp`, `IFE_VTables.hpp`, `IFE_FieldKeys.hpp`, WASM stubs | ✅ landed (PR #3) |
| 4 | Universal 10-byte `DATA_BLOCK` header, `Builder::amend_pointer` | ✅ landed (this PR) |
| 5 | `IFE_ARRAY` block, `KIND_AND_STEP`, `Span<T>` (removes `std::vector` from substrate read paths) | ✅ landed (this PR) |
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

### Phase 1 follow-up — sparse-VMA storage substrate

Phase 1 originally shipped with a heap-backed `detail::Body`
(`::operator new` with aligned allocation). This was a placeholder so the
Handle/Body + lock-free cursor protocol could land without OS-mapping
plumbing. The substrate has since been switched to the same sparse VMA
that backs `FF_Memory_t` in FastFHIR:

* **POSIX**: anonymous mode is `mmap(MAP_PRIVATE | MAP_ANONYMOUS)`;
  file-backed mode is `open(O_RDWR | O_CREAT)` + `ftruncate(capacity)` +
  `mmap(MAP_SHARED, fd, 0)`.
* **Windows**: anonymous mode is `CreateFileMappingA(INVALID_HANDLE_VALUE,
  …, PAGE_READWRITE)` + `MapViewOfFile`; file-backed mode is `CreateFileA`
  + `FSCTL_SET_SPARSE` + `CreateFileMappingA(hFile, …)` + `MapViewOfFile`.
* `Body` carries OS handles (`int m_os_fd` on POSIX; `void* m_os_handle,
  m_file_handle` on Windows) and `munmap`/`UnmapViewOfFile` + `close`/
  `CloseHandle` in the destructor. Mirroring FastFHIR, named SHM segments
  would deliberately not be `shm_unlink`'d so a restarting service can
  re-attach.
* Existing `Memory::create(capacity)` signature is preserved — it now
  returns an anonymous, sparse arena (kernel commits pages on first touch,
  so a 4 GiB reservation costs ~0 RSS until written).
* New `Memory::createFromFile(path, capacity)` factory mirrors FastFHIR.
  The encoded IFE container *is* the file: writes flow through the page
  cache, no `write(2)` after the fact.
* New `Memory::truncate_file(size)` is a no-op on anonymous arenas and
  calls `ftruncate` / `SetEndOfFile` on file-backed arenas. Used at
  finalize to trim the sparse reservation back to the actually-written
  extent (typically `write_head()`).
* Heap-allocator alignment scaffolding (the
  `max(alignof(uint64_t), atomic_ref::required_alignment)` formula and
  its `static_assert`s) was dropped: `mmap` and `MapViewOfFile` return at
  minimum page-aligned pointers, which trivially satisfies any conforming
  `atomic_ref<uint64_t>::required_alignment`. A single
  `static_assert(std::atomic_ref<uint64_t>::required_alignment <= 4096)`
  documents the assumption.
* `tests/ife_memory_tests.cpp` adds `test_file_backed_persistence`, which
  proves zero-copy persistence: `createFromFile(tmp, cap)` →
  `claim_space` → write sentinel → drop the `Memory` handle → reopen the
  path with `std::ifstream` and verify the sentinel bytes at the expected
  offset.

**Deferred follow-ups** (intentionally not in this change):
* Named POSIX SHM (`shm_open` + `ftruncate` + `mmap(MAP_SHARED)`) and the
  matching `Memory::create(capacity, std::string shm_name)` overload.
  Older glibc requires `-lrt`, so this is held back until there is an
  internal consumer.
* Read-mode `createFromFile` (validate the on-disk header and resume the
  cursor instead of re-seeding it) — lands with the Phase 6
  `Reflective::Node` recovery path.

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

## Phase 4 — what landed

Phase 4 introduces the substrate-side block builder and the universal
10-byte `DATA_BLOCK` header that every IFE block leads with. The schema
remains the authoritative source from Phase 3 onward.

### Universal preamble

In v1 every block other than `FILE_HEADER` already led with a uniform
`VALIDATION (uint64) + RECOVERY (uint16)` 10-byte preamble. `FILE_HEADER`
was the lone anomaly — it led with `MAGIC_BYTES_OFFSET (uint32) +
RECOVERY (uint16)`, a 6-byte preamble. Phase 4 folds it into the
universal shape:

| Offset | Size | Field             | Notes                                               |
|-------:|-----:|-------------------|-----------------------------------------------------|
|      0 |    8 | VALIDATION        | Holds `IFE_FILE_MAGIC` for the first block.         |
|      8 |    2 | RECOVERY          | `RESOURCE_HEADER`.                                  |
|     10 |    2 | EXTENSION_MAJOR   |                                                     |
|     12 |    2 | EXTENSION_MINOR   |                                                     |
|     14 |    4 | FILE_REVISION     |                                                     |
|     18 |    8 | FILE_SIZE         |                                                     |
|     26 |    8 | TILE_TABLE_OFFSET |                                                     |
|     34 |    8 | METADATA_OFFSET   |                                                     |

`IFE_FILE_MAGIC` is a 64-bit zero-extension of the legacy 32-bit
`MAGIC_BYTES = 0x49726973`, so any code that probes the first 4 bytes of
an `.iris` file via `LOAD_U32` continues to recognise the magic
unchanged. Every other resource is unchanged — they already conformed
to the universal preamble. The `tests/ife_codegen_tests.cpp` legacy
parity asserts therefore still pass for every resource other than
`FILE_HEADER`, whose layout asserts have been updated to reference the
universal preamble (the recovery-tag value itself is unchanged).

### Substrate API additions

* `src/IFE_DataBlock.hpp` — universal 10-byte block header type:
  * `IFE::DATA_BLOCK_HEADER_SIZE`, `IFE::DATA_BLOCK_VALIDATION_OFFSET`,
    `IFE::DATA_BLOCK_RECOVERY_OFFSET`.
  * `IFE::IFE_FILE_MAGIC` constant (`static_assert`-pinned to match the
    legacy `MAGIC_BYTES` in its low 32 bits and to match the legacy
    `IrisCodec::Serialization::DATA_BLOCK::HEADER_SIZE` for its size).
  * `IFE::DataBlockHeader` POD + `read_header(buf)` / `write_header(buf,
    tag, validation)` / `validate_at(view, offset, expected_tag)` /
    `is_file_magic(buf)` helpers.

* `src/IFE_Builder.hpp` / `src/IFE_Builder.cpp` — Builder skeleton:
  * `IFE::Builder(IFE::Memory)` / `Builder::create(capacity)` factory.
  * `BlockHandle claim_block(tag, body_bytes, validation = 0)` — claims
    `10 + body_bytes` via `Memory::claim_space` and writes the universal
    preamble. Returns `{offset, body_offset, body, body_size, tag}`.
  * `BlockHandle claim_file_header(body_bytes)` — convenience wrapper
    that pre-fills `validation = IFE_FILE_MAGIC` and `tag =
    RESOURCE_HEADER`.
  * `void amend_pointer(slot_offset, child_offset)` — release-ordered
    atomic 64-bit store into a previously-claimed parent slot. This is
    the lock-free forward-pointer fixup; concurrent readers either see
    zero (slot unset) or the valid child offset.
  * `uint64_t read_pointer(slot_offset)` — acquire-ordered counterpart
    used by tests (production code reads via the reflective lens added
    in Phase 6).
  * Bounds-checked: `amend_pointer` / `read_pointer` throw
    `std::out_of_range` if the slot would extend past the arena.

### Tests

* `tests/ife_datablock_tests.cpp` (new):
  * Universal-preamble round-trip.
  * `IFE_FILE_MAGIC` low half == legacy `MAGIC_BYTES`; arena bytes
    spell `'sirI'` (legacy magic, little-endian) followed by zeros.
  * `Builder::claim_block` lays out a valid preamble at the offset
    returned by `claim_space`; body pointer matches the in-arena
    address; arena cursor advances correctly.
  * `Builder::claim_file_header` carries the file magic and is
    recognised by `is_file_magic` and `validate_at`.
  * Lock-free 32-thread × 256-iteration `amend_pointer` race: every slot
    ends with the expected child offset, no two children land at the
    same offset. **TSan-clean.**
  * Bounds and empty-handle error paths.

* `tests/ife_codegen_tests.cpp` (updated): the FILE_HEADER legacy
  offset/size parity asserts are replaced with self-consistency asserts
  against the universal preamble (`offset::VALIDATION == 0`, size==8,
  `RECOVERY` at offset 8). Every other resource's parity asserts are
  unchanged.

### CMake

* `IFE_USE_FASTFHIR_SUBSTRATE=ON` now exports `IFE_DataBlock.hpp` /
  `IFE_Builder.hpp` and compiles `IFE_Builder.cpp`.
* New `ife_datablock_tests` ctest entry; links `Threads::Threads` for
  the multi-thread amend race.

### Substrate-OFF dormancy preserved

Substrate-OFF builds are **byte-identical** to before this PR — none of
the new headers or sources are compiled, no Python is invoked, and the
legacy `IrisCodec::Abstraction::File` write path still produces the same
on-disk bytes as before.

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

## Phase 5 — what landed

Phase 5 introduces a self-describing `IFE_ARRAY` block on top of the
Phase 4 universal preamble, plus a zero-copy `Span<T>` read path that
replaces `std::vector<T>` in substrate read code.

### IFE_ARRAY layout

| Offset | Size | Field            | Notes                                              |
|-------:|-----:|------------------|----------------------------------------------------|
|      0 |    8 | VALIDATION       | DATA_BLOCK preamble (Phase 4).                     |
|      8 |    2 | RECOVERY         | An `ARRAY_*` tag (e.g. `ARRAY_UINT64`).            |
|     10 |    2 | KIND_AND_STEP    | High byte = `IFE::IFE_FieldKind`; low byte = element wire size in bytes (1..255). |
|     12 |    4 | COUNT            | `uint32_t` element count.                          |
|     16 |    … | ELEMENTS         | `count * step` contiguous element bytes.           |

Total header = 16 bytes (`IFE::IFE_ARRAY_HEADER_SIZE`). The
`KIND_AND_STEP` field lets a reader self-validate an array's element
type against `IFE::TypeTraits<T>` without external schema metadata,
which also gives the Phase 6 reflective lens a cheap path to walk an
unknown array.

### Substrate API additions

* `src/IFE_Array.hpp`:
  * `IFE::IFE_ARRAY_HEADER_SIZE`, `IFE::IFE_ARRAY_KIND_AND_STEP_OFFSET`,
    `IFE::IFE_ARRAY_COUNT_OFFSET`, `IFE::IFE_ARRAY_MAX_STEP` constants.
  * `pack_kind_and_step(kind, step)` / `unpack_kind_and_step(u16)` —
    `constexpr` round-trip helpers; `static_assert`s pin the layout.
  * **`IFE::Span<T>`** — header-only, non-owning `{ const T*, size_t }`
    pair with `std::vector`-shaped accessors (`size`, `empty`, `data`,
    `operator[]`, `at`, `front`, `back`, `begin`/`end`). No allocation,
    no copying, trivially copyable. This is the canonical replacement
    for `std::vector<T>` in substrate read paths.
  * **`IFE::ArrayView<T>`** — read view over an `IFE_ARRAY` block in an
    arena. Construction validates the universal preamble tag against
    `TypeTraits<T>::array_tag` and the `KIND_AND_STEP` against
    `TypeTraits<T>::kind` / `TypeTraits<T>::wire_size`; throws
    `std::invalid_argument` on mismatch or OOB. Exposes `size()`,
    `empty()`, `step_bytes()`, `at(i) → T` (memcpy-loaded, always
    alignment-safe), and `as_span() → Span<T>` (zero-copy fast path
    that returns a non-empty span only when `sizeof(T) == step` and the
    body pointer is naturally aligned for `T`).

* `src/IFE_Builder.hpp`:
  * `Builder::ArrayHandle<T>` — `{ offset, body_offset, body, count,
    step_bytes }`, with `write(i, v)` writing element `i` via memcpy.
  * `Builder::claim_array<T>(count) → ArrayHandle<T>` — claims
    `IFE_ARRAY_HEADER_SIZE + count * TypeTraits<T>::wire_size` via
    `Memory::claim_space`, writes the universal preamble (with
    `tag = TypeTraits<T>::array_tag`), `KIND_AND_STEP`, and `count`,
    then leaves the body for the caller to fill. Lock-free with
    concurrent claims (the underlying space reservation is the same
    `fetch_add`). Throws `std::overflow_error` on `count * step`
    overflow, `std::bad_alloc` on arena exhaustion.

### Naming change: `IFE::Span` → `IFE::Reservation`

The Phase 1 reservation handle returned by `Memory::claim_space` was
named `IFE::Span` — but with Phase 5 introducing the canonical templated
`IFE::Span<T>` view, the writable reservation handle is renamed to
`IFE::Reservation` (same fields: `{offset, size, ptr}`). This is a
substrate-internal rename only; no on-disk format change, no public ABI
change for the legacy `IrisCodec::Abstraction::File` API.

### Tests

`tests/ife_array_tests.cpp` (new):
* `Span<T>` invariants (empty, accessors, iteration, bounds-checked
  `at`).
* `KIND_AND_STEP` round-trip with byte layout pin.
* `Builder::claim_array` round-trip for `uint64_t`, `float`, and
  `LayerExtentBlock` (the BLOCK datatype) — including `as_span()`
  zero-copy path.
* Empty arrays (`count == 0`) round-trip cleanly.
* `ArrayView<T>` rejects tag mismatch (constructing `ArrayView<float>`
  over a `uint64_t` array) and step mismatch (corrupted KIND_AND_STEP).
* OOB construction throws.
* 16-thread × 64-iteration concurrent `claim_array` race: every array
  reads back its expected contents; no two arrays share an offset.
  **TSan-clean.**
* `count * step` overflow throws `std::overflow_error`.

### CMake

* Substrate-ON exports `IFE_Array.hpp` alongside the Phase 1-4 headers.
* New `ife_array_tests` ctest entry (links `Threads::Threads`).

### Substrate-OFF dormancy preserved

Substrate-OFF builds remain byte-identical to before this PR.

## Open items blocking Phase 6

These items must be resolved before Phase 6 can proceed; they are tracked
on the parent migration tracking issue:

1. Pointer to a real FastFHIR repository (`FF_Memory`, `ffc.py`). Phase 1
   inferred semantics from the migration spec; later phases need the source.
2. Confirmation of `Abstraction::File` ABI break in Phase 6.
3. A small corpus of real `.iris` files for round-trip parity tests.
