/**
 * @file IFE_Builder.hpp
 * @brief Lock-free block builder over the IFE virtual memory arena.
 *        Phase 4 of the FastFHIR substrate migration.
 *
 * The Builder is a thin orchestrator on top of `IFE::Memory` (Phase 1) and
 * the universal `DATA_BLOCK` header (Phase 4). It is intentionally **state-
 * less per call**: every method only reads/writes the arena and is safe to
 * call concurrently from multiple ingestion threads. There is no per-Builder
 * lock; concurrent block claims serialise through `Memory::claim_space`'s
 * single `fetch_add`, and `amend_pointer` is a release-ordered atomic store.
 *
 * Typical usage (single-threaded outline):
 *
 *     auto mem = IFE::Memory::create(8 * 1024 * 1024);
 *     IFE::Builder b{mem};
 *     auto file_hdr   = b.claim_file_header(/.../);
 *     auto tile_table = b.claim_block(IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE,
 *                                     tile_table_body_bytes);
 *     b.amend_pointer(file_hdr.offset_of(file_hdr_tile_table_slot),
 *                     tile_table.offset);
 *
 * `BlockHandle::offset` is the absolute arena offset of the block's first
 * byte (the start of the universal preamble). `BlockHandle::body` and
 * `body_offset` skip past the 10-byte preamble for callers that want to
 * write the resource-specific fields directly.
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#ifndef IFE_Builder_hpp
#define IFE_Builder_hpp

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "IFE_Array.hpp"
#include "IFE_DataBlock.hpp"
#include "IFE_Memory.hpp"
#include "IFE_Types.hpp"

namespace IFE {

/// Result of a successful `Builder::claim_block` / `claim_file_header`.
/// `offset` is the absolute arena offset of the universal preamble's first
/// byte; `body_offset` and `body` skip past the 10-byte preamble.
struct BlockHandle {
    std::uint64_t  offset       = 0;       ///< Absolute arena offset of the preamble.
    std::uint64_t  body_offset  = 0;       ///< == offset + DATA_BLOCK_HEADER_SIZE.
    std::uint8_t*  body         = nullptr; ///< Direct pointer to the body bytes (in-arena).
    std::size_t    body_size    = 0;       ///< Length of the body in bytes.
    RECOVERY_TAG   tag          = RECOVERY_TAG::UNDEFINED;

    constexpr bool valid() const noexcept { return body != nullptr; }
};

class Builder {
public:
    /// Construct a builder over an existing `Memory` handle. The handle must
    /// be non-empty; passing a default-constructed `Memory{}` makes every
    /// subsequent claim throw `std::logic_error`.
    explicit Builder(Memory mem) noexcept : m_mem(std::move(mem)) {}

    /// Convenience: allocate a fresh arena of `capacity` bytes and wrap it.
    static Builder create(std::size_t capacity) {
        return Builder{Memory::create(capacity)};
    }

    /// True if the underlying arena handle is non-empty.
    explicit operator bool() const noexcept { return static_cast<bool>(m_mem); }

    /// Underlying arena handle. Useful for callers that need a `Memory::View`
    /// or want to read back what the builder has written.
    const Memory& memory() const noexcept { return m_mem; }
    Memory&       memory() noexcept       { return m_mem; }

    /**
     * @brief Claim `DATA_BLOCK_HEADER_SIZE + body_bytes` of arena space and
     *        write the universal preamble. Body bytes are zero-initialised by
     *        the underlying arena allocation; the caller fills them in.
     *
     * Thread-safe with concurrent `claim_block` / `claim_space` calls because
     * the underlying space reservation is a single `fetch_add`.
     *
     * @throws std::bad_alloc   if the arena is exhausted.
     * @throws std::logic_error if `body_bytes == 0` and the caller asked for
     *                          a header-only block, or this builder is empty.
     */
    BlockHandle claim_block(RECOVERY_TAG  tag,
                            std::size_t   body_bytes,
                            std::uint64_t validation = 0);

    /**
     * @brief Claim the FILE_HEADER block. Identical to `claim_block` but with
     *        `tag = RESOURCE_HEADER` and the validation slot pre-filled with
        *        `IFE_FILE_MAGIC` so the file's first 4 bytes identify the
     *        IFE wire format.
     *
     * @param body_bytes  Body length excluding the 10-byte universal preamble.
     *                    Pass `0` to claim only the preamble (rare; usually a
     *                    real FILE_HEADER body has 32 bytes of pointers/version
     *                    fields per `schema/ife_v1.json`).
     */
    BlockHandle claim_file_header(std::size_t body_bytes);

    /**
     * @brief Lock-free forward-pointer fixup. Stores `child_offset` (an
     *        absolute arena offset) into the 8-byte slot at `slot_offset`,
     *        using a release-ordered atomic 64-bit store so concurrent
     *        readers either see zero (slot unset) or the valid child offset —
     *        never a torn write.
     *
     * Bounds-checked: throws `std::out_of_range` if the slot would extend
     * past the arena's capacity.
     *
     * @throws std::logic_error  if this builder is empty.
     * @throws std::out_of_range if `slot_offset + 8 > capacity`.
     */
    void amend_pointer(std::uint64_t slot_offset, std::uint64_t child_offset);

    /**
     * @brief Read back the 8-byte slot at `slot_offset` with acquire ordering.
     *        Mainly used by tests; production code reads via the reflective
     *        lens added in Phase 6.
     */
    std::uint64_t read_pointer(std::uint64_t slot_offset) const;

    // -------------------------------------------------------------------
    // Phase 5 — IFE_ARRAY block construction
    // -------------------------------------------------------------------

    /// Writable handle returned by `claim_array<T>`. The caller fills `body`
    /// with `count` elements (each `TypeTraits<T>::wire_size` bytes wide) in
    /// any order; the header has already been written by `claim_array`.
    template <class T>
    struct ArrayHandle {
        std::uint64_t  offset      = 0;       ///< Absolute arena offset of the preamble.
        std::uint64_t  body_offset = 0;       ///< == offset + IFE_ARRAY_HEADER_SIZE.
        std::uint8_t*  body        = nullptr; ///< First element byte (in-arena).
        std::size_t    count       = 0;       ///< Element count.
        std::size_t    step_bytes  = 0;       ///< == TypeTraits<T>::wire_size.

        constexpr bool valid() const noexcept { return body != nullptr; }

        /// Write element `i` from a `T` value via `std::memcpy` so the
        /// caller never has to think about alignment. `i` must be in range.
        void write(std::size_t i, const T& v) noexcept {
            std::memcpy(body + i * step_bytes, &v, step_bytes);
        }
    };

    /**
     * @brief Stamp the FILE_HEADER's `FILE_SIZE` slot with the current
     *        write head and trim the underlying file (file-backed mode) to
     *        that length. Phase 6a canonical commit point for the Phase 1
     *        sparse-VMA work.
     *
     * The Builder remembers the FILE_HEADER block offset captured by the
     * most recent `claim_file_header` call. `finalize` locates the
     * `FILE_SIZE` slot via the codegen vtable
     * (`vtables::FILE_HEADER::offset::FILE_SIZE`) and writes the current
     * `write_head()` there using a release-ordered atomic 64-bit store
     * (same path as `amend_pointer`). It then calls
     * `Memory::truncate_file(write_head())` — a no-op for anonymous
     * arenas, an `ftruncate` / `SetEndOfFile` for file-backed arenas.
     *
     * Idempotent: calling `finalize` multiple times re-stamps the FILE_SIZE
     * slot and re-issues the truncate, which is harmless. Calling other
     * `claim_*` methods after `finalize` is allowed and well-defined; a
     * subsequent `finalize` will pick up the new write head.
     *
     * @return The committed file size (== `write_head()` at call time).
     *
     * @throws std::logic_error  if no `claim_file_header` has been issued on
     *                           this Builder, or this builder is empty.
     * @throws std::system_error on `ftruncate` / `SetEndOfFile` failure.
     */
    std::uint64_t finalize();

    /// Absolute arena offset of the FILE_HEADER block, or 0 if no
    /// `claim_file_header` has been issued.
    std::uint64_t file_header_offset() const noexcept {
        return m_file_header_offset;
    }

    /**
     * @brief Claim an `IFE_ARRAY` block carrying `count` elements of type T.
     *        Writes the universal preamble, `KIND_AND_STEP`, and `count`;
     *        leaves the body zero-initialised for the caller to fill.
     *
     * The wire size used per element is `TypeTraits<T>::wire_size`, which
     * may differ from `sizeof(T)` for narrow on-disk encodings (uint24 etc).
     * The Phase 5 supported types are the primitives + `LayerExtentBlock`,
     * for which `sizeof(T) == wire_size`.
     *
     * Thread-safe with concurrent `claim_block` / `claim_array` / `claim_space`
     * calls — the underlying space reservation is a single `fetch_add`.
     *
     * @throws std::bad_alloc   if the arena is exhausted.
     * @throws std::logic_error if this builder is empty.
     * @throws std::overflow_error if `count * wire_size` would overflow.
     */
    template <class T>
    ArrayHandle<T> claim_array(std::size_t count);

private:
    Memory        m_mem;
    /// Absolute arena offset of the FILE_HEADER block, captured by
    /// `claim_file_header` and consumed by `finalize`. Zero when unset
    /// (a real FILE_HEADER never lands at offset 0 because the first
    /// `WRITE_HEAD_BYTES = 16` bytes are reserved for the atomic cursor).
    std::uint64_t m_file_header_offset = 0;
};

// =============================================================================
// claim_array<T> — header-implemented because it's a template
// =============================================================================

template <class T>
Builder::ArrayHandle<T> Builder::claim_array(std::size_t count) {
    if (!m_mem) {
        throw std::logic_error("IFE::Builder::claim_array: empty Memory handle");
    }
    constexpr std::size_t step = TypeTraits<T>::wire_size;
    static_assert(step <= IFE_ARRAY_MAX_STEP,
                  "TypeTraits<T>::wire_size exceeds IFE_ARRAY step byte range");

    // Overflow guard: count * step + header must fit in size_t.
    const std::size_t max_count =
        (static_cast<std::size_t>(-1) - IFE_ARRAY_HEADER_SIZE) / step;
    if (count > max_count) {
        throw std::overflow_error("IFE::Builder::claim_array: count overflow");
    }
    const std::size_t total = IFE_ARRAY_HEADER_SIZE + count * step;

    auto reservation = m_mem.claim_space(total);
    // Universal preamble.
    write_header(reservation.ptr, TypeTraits<T>::array_tag, /*validation=*/0);
    // KIND_AND_STEP.
    const std::uint16_t kas =
        pack_kind_and_step(TypeTraits<T>::kind, step);
    std::memcpy(reservation.ptr + IFE_ARRAY_KIND_AND_STEP_OFFSET, &kas,
                sizeof(kas));
    // count.
    const std::uint32_t count32 = static_cast<std::uint32_t>(count);
    std::memcpy(reservation.ptr + IFE_ARRAY_COUNT_OFFSET, &count32,
                sizeof(count32));

    ArrayHandle<T> h;
    h.offset      = reservation.offset;
    h.body_offset = reservation.offset + IFE_ARRAY_HEADER_SIZE;
    h.body        = reservation.ptr + IFE_ARRAY_HEADER_SIZE;
    h.count       = count;
    h.step_bytes  = step;
    return h;
}

}  // namespace IFE

#endif  // IFE_Builder_hpp
