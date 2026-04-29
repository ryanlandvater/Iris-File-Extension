/**
 * @file IFE_Builder.cpp
 * @brief Implementation of `IFE::Builder`. Phase 4 of the substrate migration.
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#include "IFE_Builder.hpp"

#include <atomic>
#include <stdexcept>
#include <string>

namespace IFE {

namespace {

/// Compute the absolute pointer to a `slot_offset` byte run after bounds-
/// checking against the arena capacity. Returns `nullptr` if the handle is
/// empty; throws `std::out_of_range` on a real OOB.
std::uint8_t* slot_ptr_checked(Memory&       mem,
                               std::uint64_t slot_offset,
                               std::size_t   slot_bytes,
                               const char*   what) {
    if (!mem) {
        throw std::logic_error("IFE::Builder: empty Memory handle");
    }
    const auto cap = mem.capacity();
    if (slot_offset > cap || cap - slot_offset < slot_bytes) {
        throw std::out_of_range(std::string{"IFE::Builder: "} + what +
                                ": slot extends past arena capacity");
    }
    return mem.data() + slot_offset;
}

}  // namespace

BlockHandle Builder::claim_block(RECOVERY_TAG  tag,
                                 std::size_t   body_bytes,
                                 std::uint64_t validation) {
    if (!m_mem) {
        throw std::logic_error("IFE::Builder::claim_block: empty Memory handle");
    }
    // Underlying claim_space rejects 0-byte requests; we always need at
    // least the preamble so this is fine.
    const std::size_t total = DATA_BLOCK_HEADER_SIZE + body_bytes;
    Reservation span = m_mem.claim_space(total);
    // claim_space returns a valid pointer or throws.
    write_header(span.ptr, tag, validation);

    BlockHandle h;
    h.offset      = span.offset;
    h.body_offset = span.offset + DATA_BLOCK_HEADER_SIZE;
    h.body        = span.ptr + DATA_BLOCK_HEADER_SIZE;
    h.body_size   = body_bytes;
    h.tag         = tag;
    return h;
}

BlockHandle Builder::claim_file_header(std::size_t body_bytes) {
    return claim_block(RECOVERY_TAG::RESOURCE_HEADER,
                       body_bytes,
                       IFE_FILE_MAGIC);
}

void Builder::amend_pointer(std::uint64_t slot_offset,
                            std::uint64_t child_offset) {
    std::uint8_t* p = slot_ptr_checked(m_mem, slot_offset,
                                       sizeof(std::uint64_t),
                                       "amend_pointer");
    // Release store so any prior writes to the child block (header + body)
    // happen-before any reader that loads this slot with acquire ordering
    // and follows the offset. The slot is plain `uint64_t` storage in the
    // arena; access goes through `std::atomic_ref<u64>` (project rule:
    // never alias raw arena bytes as `std::atomic<T>*`).
    auto* slot = reinterpret_cast<std::uint64_t*>(p);
    std::atomic_ref<std::uint64_t>(*slot)
        .store(child_offset, std::memory_order_release);
}

std::uint64_t Builder::read_pointer(std::uint64_t slot_offset) const {
    // const-cast is safe: we only read with acquire ordering. The arena
    // bytes are mutable through the handle even when the handle is const.
    Memory& mut = const_cast<Memory&>(m_mem);
    std::uint8_t* p = slot_ptr_checked(mut, slot_offset,
                                       sizeof(std::uint64_t),
                                       "read_pointer");
    auto* slot = reinterpret_cast<std::uint64_t*>(p);
    return std::atomic_ref<std::uint64_t>(*slot)
        .load(std::memory_order_acquire);
}

}  // namespace IFE
