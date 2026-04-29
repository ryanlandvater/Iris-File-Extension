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
 * This header is **dormant** unless built with `IFE_USE_FASTFHIR_SUBSTRATE`.
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#ifndef IFE_Builder_hpp
#define IFE_Builder_hpp

#include <cstddef>
#include <cstdint>

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
     *        `IFE_FILE_MAGIC` so the file's first 8 bytes identify the
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

private:
    Memory m_mem;
};

}  // namespace IFE

#endif  // IFE_Builder_hpp
