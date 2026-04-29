# IFE 2.x Migration — FastFHIR Substrate

> **Status:** in progress (Phases 1-6a landed; Phase 6b partially landed — accessors + read-mode factory + deprecation in this PR; ON-by-default cutover and `IrisCodecExtension.cpp` regeneration remain blocked on the parity corpus). Targeted version: `2.0.0-alpha`.

The Iris File Extension is migrating from the eager-mapping `Abstraction::File`
pattern to a lock-free Virtual Memory Arena (VMA) substrate ported from
FastFHIR. Goals:

* Zero-copy reads via reflective lenses (`Reflective::Node` / `Entry`).
* Lock-free concurrent block ingestion (`claim_space` via `fetch_add`).
* Schema-driven codegen (`ifc.py`) replacing hand-written byte-offset enums.
* A universal 10-byte `DATA_BLOCK` header for all blocks on disk.
* **Fresh design**, not a bridge: the substrate is its own format, pinned
  by its own `static_assert`s. The cleanup pass landed alongside Phase 6a
  removed every `IrisCodec::Serialization::*` reference from the substrate
  headers (`legacy_to_tag` / `tag_to_legacy`, the `IFE_ROUND_TRIP` macro,
  the wire-size and header-size parity asserts, and the
  `IrisFileExtension.hpp` includes from `IFE_Types.hpp` / the substrate
  test set). See [Cleanup pass](#cleanup-pass--what-dropped) below.

The migration is staged across **six sequential PRs**. Each PR is independently
reviewable, buildable, and testable. The legacy `IrisCodecExtension.{hpp,cpp}`
API continues to build and behave unchanged until **Phase 6b**.

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
| 6a | `IFE_Reflective::Node`/`Entry` zero-copy lens, `Builder::finalize()` | ✅ landed (this PR) |
| 6b (partial) | Codegen `IFE_Accessors.hpp`, `Memory::openFromFile`, `[[deprecated]]` on `Abstraction::File` + `STORE_*` helpers | ✅ landed (this PR) |
| 6b (cutover) | Flip `IFE_USE_FASTFHIR_SUBSTRATE` default ON; delete `IrisCodecExtension.cpp` and regenerate it from `schema/ife_v1.json` | pending parity corpus |

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
    the tag carried by every `Reflective::Entry`.
  * Partition constants: `RECOVERY_PARTITION_PRIMITIVE` (`0x01xx`),
    `RECOVERY_PARTITION_DATATYPE` (`0x02xx`),
    `RECOVERY_PARTITION_RESOURCE` (`0x03xx`), and the array bit
    `RECOVERY_ARRAY_BIT = 0x8000` with the `IsArrayTag(uint16_t)` predicate.
  * `enum class RECOVERY_TAG : uint16_t` covering every v1 resource
    (HEADER, TILE_TABLE, METADATA, ATTRIBUTES, LAYER_EXTENTS,
    TILE_OFFSETS, ATTRIBUTES_SIZES, ATTRIBUTES_BYTES, ASSOCIATED_IMAGES,
    ASSOCIATED_IMAGE_BYTES, ICC_PROFILE, ANNOTATIONS, ANNOTATION_BYTES,
    ANNOTATION_GROUP_SIZES, ANNOTATION_GROUP_BYTES, CIPHER, UNDEFINED)
    plus primitive scalars (`uint8/16/32/64`, `int8/16/32/64`, `float`,
    `double`) and the `LAYER_EXTENT` BLOCK datatype. Every ARRAY variant
    is pre-OR'd with `0x8000`.
  * `static_assert`s pin every tag to its expected partition and numeric
    value, so a future schema edit cannot silently re-number a tag.
  * `template<class T> struct TypeTraits` specializations for the primitives
    the codebase actually serializes today, plus `LayerExtentBlock` as the
    first BLOCK datatype. Wire sizes are pinned against `sizeof(T)` and
    against the summed schema field widths — the substrate is its own
    source of truth for the on-disk encoding.
* `tests/ife_types_tests.cpp` — runtime checks for the partition helpers
  and `TypeTraits` specializations. Most invariants are already pinned at
  compile time inside `IFE_Types.hpp`.

### Dormant by design

Phase 1 introduces **no on-disk format change** and **no public ABI change**.
The new headers are only compiled when `IFE_USE_FASTFHIR_SUBSTRATE=ON`, and
even then they are not wired into `Abstraction::File` or
`validate_file_structure`. Wiring happens in Phase 6b. Phases 2-3 likewise
add only header-only metadata (Phase 2) and build-tree-only generated
headers (Phase 3) — `IFE_Types.hpp` is exported alongside `IFE_Memory.hpp`
when the substrate flag is ON; the legacy `IrisCodec::Serialization` block
layouts continue to define the v1 wire format separately until Phase 6b
flips the default.

## Phase 3 — what landed

* `schema/ife_v1.json` — JSON description of the v1 IFE container. Single
  source of truth for every resource (FILE_HEADER, TILE_TABLE, METADATA,
  ATTRIBUTES, LAYER_EXTENTS, TILE_OFFSETS, ATTRIBUTES_SIZES,
  ATTRIBUTES_BYTES, IMAGE_ARRAY, IMAGE_BYTES, ICC_PROFILE, ANNOTATIONS,
  ANNOTATION_BYTES, ANNOTATION_GROUP_SIZES, ANNOTATION_GROUP_BYTES) and
  every sub-block datatype (LAYER_EXTENT, TILE_OFFSET, ATTRIBUTE_SIZE,
  IMAGE_ENTRY, ANNOTATION_ENTRY, ANNOTATION_GROUP_SIZE).
* `tools/ifc.py` — Python 3 stdlib-only codegen (no jinja2 dependency).
  Reads the schema and emits five C++ headers into the build tree.
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
    `IrisCodecExtension.hpp` for the substrate code paths.
  * `IFE_FieldKeys.hpp` — `constexpr std::array<FieldKey, N>` per
    resource, where `FieldKey = {name, kind, scalar_tag, offset, size}`.
    Consumed by the reflective reader.
  * **`IFE_Resources.hpp`** — single per-resource dispatch table
    `IFE::resources::table[]` of `ResourceInfo {tag, name, header_size,
    fields, field_count}` plus a `lookup(tag)` helper. The reflective
    lens does one linear scan over this table instead of the two
    parallel N-way switches it used to carry. Adding a new resource is
    a pure schema change; zero edits to `IFE_Reflective.hpp` are needed.
  * `IFE_FieldKeys_wasm.hpp` — `__EMSCRIPTEN__`-gated `extern "C"`
    surface. Stubs only.
* CMake integration (`IFE_USE_FASTFHIR_SUBSTRATE=ON`):
  * Requires Python 3 (`find_package(Python3 REQUIRED)`).
  * Runs `ifc.py` at configure time; `CMAKE_CONFIGURE_DEPENDS` re-runs
    it if either the schema or the codegen script changes.
  * Adds the build-tree generated dir to the include path.
* `tests/ife_codegen_tests.cpp` — codegen-output sanity test. Verifies the
  generated headers are *internally consistent*: schema version pin,
  FILE_HEADER preamble layout, monotonic field offsets, `header_size`
  equals the universal preamble plus the sum of field sizes, dispatch
  table round-trip via `lookup`, and every entry's tag in the RESOURCE
  partition. The codegen output is the source of truth — there is no
  longer a parity check against the legacy `IrisCodec::Serialization`
  enums. A second ctest entry (`ife_codegen_check`) re-runs
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
- The substrate is independent of the legacy
  `IrisCodec::Serialization` enums. The legacy enums in
  `IrisCodecExtension.hpp` continue to drive the legacy write path
  (substrate-OFF builds) until Phase 6b retires it.

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

`IFE_FILE_MAGIC` packs the four ASCII bytes `'I','r','i','s'` into the
low 32 bits of the validation slot (so a `LOAD_U32` at file offset 0
reads `0x49726973`). Every other resource was already conformant. Both
the magic and `DATA_BLOCK_HEADER_SIZE` are pinned by `static_assert` in
`IFE_DataBlock.hpp`.

### Substrate API additions

* `src/IFE_DataBlock.hpp` — universal 10-byte block header type:
  * `IFE::DATA_BLOCK_HEADER_SIZE`, `IFE::DATA_BLOCK_VALIDATION_OFFSET`,
    `IFE::DATA_BLOCK_RECOVERY_OFFSET`.
  * `IFE::IFE_FILE_MAGIC` constant (`static_assert`-pinned to spell
    `'I','r','i','s'` in its low 32 bits).
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
  * `IFE_FILE_MAGIC` byte layout pin (arena bytes spell `'s','i','r','I'`
    little-endian; high half is zero).
  * `Builder::claim_block` lays out a valid preamble at the offset
    returned by `claim_space`; body pointer matches the in-arena
    address; arena cursor advances correctly.
  * `Builder::claim_file_header` carries the file magic and is
    recognised by `is_file_magic` and `validate_at`.
  * Lock-free 32-thread × 256-iteration `amend_pointer` race: every slot
    ends with the expected child offset, no two children land at the
    same offset. **TSan-clean.**
  * Bounds and empty-handle error paths.

* `tests/ife_codegen_tests.cpp` (rewritten as part of the cleanup pass):
  the FILE_HEADER preamble layout is pinned via the universal preamble
  (`offset::VALIDATION == 0`, size==8, `RECOVERY` at offset 8). Every
  resource is then walked via the codegen `IFE::resources::table`
  dispatch and asserted to be internally consistent (monotonic offsets,
  `header_size == preamble + sum(field sizes)`, tag in RESOURCE
  partition). No external parity baseline.

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

## Phase 6a — what landed

Phase 6a introduces the **reflective read lens** and the **canonical
write-finalize** step. Together they close the loop on the substrate write
path opened by Phases 1–5: a builder thread can now claim blocks, wire
forward pointers, finalize the FILE_HEADER + arena, and a reader thread
can walk the resulting bytes purely through Phase 1 `Memory::View`,
Phase 4 `DATA_BLOCK` preamble, Phase 5 `IFE_ARRAY` headers, and the
Phase 3 codegen catalogs — without touching the legacy
`IrisCodec::Abstraction::File` API.

Phase 6a is **additive and substrate-OFF byte-identical**. The ABI-breaking
pieces of the original Phase 6 (deprecating `Abstraction::File`, flipping
the `IFE_USE_FASTFHIR_SUBSTRATE` default to ON) are deferred to Phase 6b,
which is gated on the open blockers above.

### Reflective lens (`IFE_Reflective.hpp`)

* `IFE::Reflective::Entry` — POD record per field:
  `{string_view name, IFE_FieldKind kind, RECOVERY_TAG scalar_tag,
   uint64 offset /* absolute arena offset */, size_t size}`.

* `IFE::Reflective::Node` — non-owning view over a single block:
  * Constructed from `(Memory::View, block_offset)`. Reads and validates
    the universal preamble; rejects offsets that don't fit, with bounded
    body checks routed through the Phase 3 codegen vtables for
    fixed-layout resources and through the Phase 5 `KIND_AND_STEP`
    header for `IFE_ARRAY` blocks.
  * Holds the `Memory::View` (i.e. a `shared_ptr<const Body>`) so the
    arena cannot be unmapped while a `Node` exists — same lifetime
    contract as every other `Memory::View`-based reader.
  * `recovery_tag()`, `validation()`, `block_offset()`, `body_offset()`,
    `body_size_hint()`, `entry_count()`, `entries()` are O(1) noexcept
    accessors after construction.
  * `field<T>(name)` — bounds-checked, alignment-safe `memcpy` load of a
    fixed-layout scalar; size-validates against the codegen `FieldKey`.
  * `pointer(name)` — convenience for `field<uint64_t>` over an 8-byte
    forward-pointer slot.
  * `array<T>(name)` — follows an 8-byte forward-pointer slot to a child
    `IFE_ARRAY<T>` and returns `IFE::ArrayView<T>`. Tag/kind/step
    validation is delegated to the Phase 5 `ArrayView` constructor.
  * `child(name)` — same, but constructs a generic child `Node`.

* Iteration via `Node::begin()` / `end()` walks the per-resource
  `IFE::field_keys::<RES>::fields` array (generated by `tools/ifc.py`),
  rebasing each field offset to absolute arena coordinates so callers
  never have to add the block offset themselves.

* Tag dispatch is a single linear scan over the codegen
  `IFE::resources::table` (one entry per resource — at most a few dozen).
  Adding a resource in `schema/ife_v1.json` regenerates the table; **zero
  edits to `IFE_Reflective.hpp` are required**. This replaces what used
  to be two parallel N-way switches over `RECOVERY_TAG` (one for the
  `field_keys` span, one for the body-size hint, ~120 lines of
  hand-maintained dispatch).

### `Builder::finalize()`

* Stamps `Memory::write_head()` into the FILE_HEADER's `FILE_SIZE` slot
  via the same release-ordered atomic 64-bit store used by
  `amend_pointer` (the slot offset is taken from
  `vtables::FILE_HEADER::offset::FILE_SIZE`, with a `static_assert`
  pinning the field width to 8 bytes).
* Then calls `Memory::truncate_file(write_head())` — no-op for anonymous
  arenas, `ftruncate` / `SetEndOfFile` for file-backed arenas — so a
  finalized `.iris` file occupies exactly the bytes actually written, even
  though Phase 1 reserved a much larger sparse VMA.
* Idempotent: re-finalizing after additional `claim_*` calls re-stamps
  with the new write head. Throws `std::logic_error` if no
  `claim_file_header` has been issued (the FILE_HEADER offset is captured
  by `claim_file_header` and stored on the Builder).

### Tests (`tests/ife_reflective_tests.cpp`)

* Anonymous-arena round-trip: build a FILE_HEADER + TILE_TABLE +
  TILE_OFFSETS resource + child `IFE_ARRAY<uint64_t>`, finalize, walk
  via `Reflective::Node`. Asserts every entry's absolute offset, every
  scalar's `field<T>` round-trip, every forward pointer's `child()`
  traversal, and that `FILE_SIZE` matches `Memory::write_head()`.

* File-backed end-to-end: `createFromFile` over a temp path, populate,
  finalize, drop the handle. The on-disk file is then asserted to be
  exactly `write_head()` bytes (i.e. truncate ran), to start with the
  Phase 1 cursor in its low 63 bits, and to carry `IFE_FILE_MAGIC` +
  `RESOURCE_HEADER` at `WRITE_HEAD_BYTES`. The `FILE_SIZE` slot in the
  on-disk FILE_HEADER body matches the file size.

* `finalize` error / idempotence paths.
* `Node` construction errors (empty view, OOB, body past capacity).
* `field<T>` errors (unknown name → `out_of_range`, size mismatch →
  `invalid_argument`, NULL pointer follow-through → `out_of_range`).
* Tag-mismatch on `array<T>`: an `IFE_ARRAY<float>` cannot be read as
  `ArrayView<uint64_t>`; the Phase 5 validator catches it.
* 8-thread × 256-iteration concurrent reader stress: every thread walks
  the finished arena via independent `Reflective::Node` constructions.
  **TSan-clean.**

### CMake

* Substrate-ON exports `IFE_Reflective.hpp` alongside the Phase 1–5
  headers.
* New `ife_reflective_tests` ctest entry (links `Threads::Threads`).
* Substrate-OFF builds remain byte-identical.

## Cleanup pass — what dropped

A cleanup pass landed alongside Phase 6a in response to the observation that
the substrate work was *adding* code (≈5k lines across Phases 1–6a) without
removing the legacy boilerplate it was meant to subsume. The substrate is
not a bridge; it is a fresh design. The pass removed every legacy reference
from the substrate headers and replaced two parallel hand-written dispatch
tables with one codegen-emitted one.

What the pass dropped:

| Surface                                        | What changed                                                       |
|------------------------------------------------|--------------------------------------------------------------------|
| `IFE_Types.hpp`                                | Dropped `legacy_to_tag` / `tag_to_legacy` (~50 LOC), the `IFE_ROUND_TRIP` macro and its 17 invocations, the `static_assert` pins against `IrisCodec::Serialization::TYPE_SIZE_*` and `LAYER_EXTENT::SIZE`, and the `#include "IrisFileExtension.hpp"`. The substrate is now self-pinning. |
| `IFE_DataBlock.hpp`                            | Dropped the `static_assert` against `IrisCodec::Serialization::DATA_BLOCK::HEADER_SIZE`. The universal preamble is its own contract. |
| `IFE_Reflective.hpp`                           | Replaced the two parallel 15-case switches (`field_keys_for` + `fixed_body_size_for`, ~120 lines) with a single `IFE::resources::lookup(tag)` call into the codegen table. |
| `tools/ifc.py`                                 | Added `IFE_Resources.hpp` codegen — one `ResourceInfo` table entry per resource, one `lookup(tag)` helper. |
| `tests/ife_types_tests.cpp`                    | Rewrote without the legacy round-trip table; kept partition-helper + `TypeTraits` sanity. |
| `tests/ife_codegen_tests.cpp`                  | Dropped every `IFE_CHECK_OFFSET / SIZE / HEADER_SIZE / RECOVERY` parity macro. Codegen is the source of truth; what remains are schema invariants (FILE_HEADER preamble, monotonic offsets, header_size accounting, dispatch round-trip). |
| `tests/ife_datablock_tests.cpp`                | Renamed `test_file_magic_matches_legacy` → `test_file_magic`; dropped the `MAGIC_BYTES` cross-reference and the `IrisFileExtension.hpp` include. |

Substrate-OFF builds remain byte-identical. `IrisCodecExtension.{hpp,cpp}`
is unchanged — it remains the legacy write/read path until Phase 6b.

## Phase 6b — partial: substrate-side cutover landed

This PR lands the substrate-side work that does **not** require a parity
corpus or an ABI break, plus the codegen scaffolding that the eventual
full cutover will compose into a regenerated serializer. The flag default
stays **OFF**; substrate-OFF builds remain byte-identical to today.

### What landed in this PR

1. **Codegen-emitted `IFE_Accessors.hpp`.** `tools/ifc.py` now emits a
   sixth header containing:

   * `accessors::detail` packed-LE primitives — `load_uN_le<N>` /
     `store_uN_le<N>` (alignment-safe `memcpy`), `sign_extend_uN<N>` for
     `int24` / `int40`, plus per-type `load_uint24` / `load_uint40` /
     `load_int24` / `load_int40` / `load_float16_raw` / `load_f32` /
     `load_f64` and their `store_*` counterparts. Call sites never deal
     with bit shifts or sign extension.
   * Per-datatype `decode(buf) -> ::IFE::datatypes::<DT>` and
     `encode(buf, value)` for every entry in `schema.datatypes`
     (`LAYER_EXTENT`, `TILE_OFFSET` with `uint40`+`uint24`,
     `ATTRIBUTE_SIZE`, `IMAGE_ENTRY`, `ANNOTATION_ENTRY` with `uint24`
     plus floats, `ANNOTATION_GROUP_SIZE`).
   * Per-resource `read_<FIELD>(block_base)` / `write_<FIELD>(block_base,
     v)` inlines for every field of every resource, plus a generated
     `Record` POD and `decode(block_base) -> Record` /
     `encode(block_base, record)` resource-level round-trippers.

   These are the building blocks that a future PR will compose into the
   full per-resource serializer (the same `read_<RES>` / `write_<RES>`
   functions called out earlier in this document) — the path to the
   eventual deletion of `IrisCodecExtension.cpp` and its regeneration
   from `schema/ife_v1.json` on every build.

2. **`IFE::Memory::openFromFile(path)` read-mode factory.** Counterpart
   to `createFromFile`. Opens an existing file at its current size, maps
   it `MAP_SHARED` / `FILE_MAP_ALL_ACCESS`, validates the FILE_HEADER
   preamble (`IFE_FILE_MAGIC` + `RESOURCE_HEADER` recovery tag), and
   cross-checks the persisted FILE_SIZE against the on-disk size. The
   atomic write-head bytes are NOT zeroed, so the cursor resumes
   exactly where `Builder::finalize()` left it. Pairs end-to-end with
   `Reflective::Node` for zero-copy reads.

3. **`[[deprecated]]` on the legacy API surface.** `IrisCodec::Abstraction::File`,
   `abstract_file_structure`, `generate_file_map`, and all 13 `STORE_*`
   bytes-in helpers now carry an `IFE_LEGACY_DEPRECATED(...)` annotation
   that expands to `[[deprecated("IFE 2.x: prefer ..." )]]` only when
   built against `IFE_USE_FASTFHIR_SUBSTRATE=ON`. Substrate-OFF
   consumers see no deprecation warnings (matching the dormancy
   guarantee). Every annotation names its substrate replacement (e.g.
   `STORE_FILE_HEADER` → `IFE::Builder::claim_file_header +
   IFE::accessors::FILE_HEADER::encode`). The legacy `.cpp` opens with
   a localised `-Wdeprecated-declarations` suppression so the file's
   own internal calls don't warn against itself.

4. **Tests.** `tests/ife_accessors_tests.cpp` round-trips every datatype
   and every fixed-layout resource record through the new accessors,
   covers the `int24` / `int40` sign-extension boundary, and asserts
   reflective parity with `IFE::Reflective::Node::field<T>`.
   `tests/ife_memory_tests.cpp` gains an `openFromFile` round-trip
   (Builder → finalize → re-open via the new factory → walk with the
   reflective lens) plus rejection cases for missing / too-small /
   garbage-magic inputs.

### Why a `IrisCodecExtension.cpp` refactor is *not* in this PR

The original Phase 6b plan called for refactoring the hand-written
`STORE_*` / `read_*` paths in `IrisCodecExtension.cpp` to call the new
codegen accessors. **This is not possible without a wire-format break:**

* The substrate's wire format (`schema/ife_v1.json`, surfaced through
  `IFE_VTables.hpp` and the new `IFE_Accessors.hpp`) is a fresh design.
  The universal preamble is `VALIDATION` (uint64) at byte 0 + `RECOVERY`
  (uint16) at byte 8, exactly as documented in [Phase 4 — what landed]
  (#phase-4--what-landed) and the very first paragraph of this document.
* The legacy `IrisCodec::Serialization::FILE_HEADER` layout (and every
  other resource) leads with `MAGIC_BYTES` (uint32) at byte 0 + `RECOVERY`
  (uint16) at byte 4 — different field, different width, different
  offset.

Calling the codegen accessors from `IrisCodecExtension.cpp` would
therefore change the on-disk byte layout of every resource the legacy
file emits. That is precisely the wire-format cutover gated by
prerequisite (3) below — it cannot ship without the parity corpus that
proves byte-exact round-trip across real `.iris` files.

### What still has to happen for full Phase 6b

The remaining steps are the same four that gated Phase 6b before this
PR landed, with one item retired:

1. **FastFHIR source pointer** (`FF_Memory`, `ffc.py`). Phase 1's
   lock-free VMA semantics were inferred from the migration spec;
   the builder/reader cutover should be cross-checked against the
   upstream reference implementation before becoming the default.
2. **`Abstraction::File` ABI-break confirmation.** Today every
   consumer of this repository links against the legacy API; the
   break must be acknowledged by the dependent tree (`Iris-Codec`,
   etc.) before the deprecation graduates from a warning to a removal.
   The `[[deprecated]]` annotations from this PR are the warning step.
3. **Real-`.iris` parity corpus.** Flipping the substrate default to
   ON requires byte-exact round-trip parity against a representative
   set of `.iris` files — the same corpus that gates the eventual
   wire-format cutover.
4. ~~**Read-mode `Memory::createFromFile`.**~~ ✅ Landed in this PR
   as `Memory::openFromFile(path)`.

When (1)–(3) are unblocked, the **final** Phase 6b step is:

* **Delete `IrisCodecExtension.cpp` and regenerate it from the
  schema on every build.** Extend `tools/ifc.py` to emit
  `IFE_Serializers.hpp` / `.cpp` containing one `read_<RES>` and one
  `write_<RES>` per schema resource, generated directly from the same
  JSON schema that already drives the offset/size vtables AND the new
  per-(resource, field) accessors from this PR. The hand-written
  `IrisCodecExtension.cpp` (~3.4k lines of bespoke `STORE_U64` /
  `LOAD_U32` calls) is deleted; the public `Abstraction::File` API
  becomes a thin shim over the regenerated serializer (or is removed
  outright once the ABI break is confirmed). This is the largest net-
  LOC reduction in the migration, and the codegen-emitted accessors
  from this PR are the foundation it composes from.

* Flip `option(IFE_USE_FASTFHIR_SUBSTRATE … OFF)` to `ON`, behind a
  feature gate that downstream consumers can flip back during their
  own cutover.

* Add a `tests/ife_corpus_parity_tests.cpp` driven by the real-`.iris`
  corpus, asserting byte-exact round-trip through the regenerated
  write/read path.
