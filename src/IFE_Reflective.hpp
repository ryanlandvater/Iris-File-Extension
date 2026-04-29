/**
 * @file IFE_Reflective.hpp
 * @brief Zero-copy reflective lens over IFE blocks.
 *        Phase 6a of the FastFHIR substrate migration.
 *
 * The reflective lens is a read-only walker built on top of:
 *   - Phase 1 `IFE::Memory::View` (lifetime-pinning shared_ptr<const Body>),
 *   - Phase 4 universal `DATA_BLOCK` preamble (`IFE_DataBlock.hpp`),
 *   - Phase 5 `IFE_ARRAY` block + `IFE::ArrayView<T>` (`IFE_Array.hpp`),
 *   - Phase 3 codegen output (`IFE_VTables.hpp`, `IFE_FieldKeys.hpp`).
 *
 * `IFE::Reflective::Node` is constructed from a `(Memory::View, offset)` pair.
 * Construction reads and validates the universal preamble, then locates the
 * resource's reflective record via its `RECOVERY_TAG`. After construction:
 *
 *   * `recovery_tag()`, `validation()`, `body_offset()`, `body_size_hint()`,
 *     `entry_count()` are noexcept O(1) accessors.
 *   * `entries()` returns a `Span<Entry>` describing every field in the
 *     resource (name, kind, scalar_tag, *absolute* arena offset, size).
 *   * `field<T>(name)` returns a single field by an alignment-safe memcpy
 *     load, validated against the entry's recorded size.
 *   * `array<T>(name)` follows an 8-byte forward pointer field to a child
 *     `IFE_ARRAY<T>` block and returns an `IFE::ArrayView<T>`.
 *
 * All accessors are bounds-checked. Failures throw `std::invalid_argument`
 * (for tag/kind mismatch and OOB) or `std::out_of_range` (for unknown field
 * names / NULL forward pointers). The lens never mutates arena state.
 *
 * Lifetime: the `Node` holds the `Memory::View` (which holds a
 * `shared_ptr<const Body>`), so the arena cannot be unmapped while a `Node`
 * exists — same lifetime contract as all other `Memory::View`-based readers.
 *
 * This header is **dormant** unless built with `IFE_USE_FASTFHIR_SUBSTRATE`
 * (it pulls in the generated codegen headers, which only exist in that mode).
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#ifndef IFE_Reflective_hpp
#define IFE_Reflective_hpp

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#include "IFE_Array.hpp"
#include "IFE_DataBlock.hpp"
#include "IFE_Memory.hpp"
#include "IFE_Types.hpp"

// Generated headers (Phase 3). `IFE_Resources.hpp` provides a single
// dispatch table `IFE::resources::table` (one entry per resource, with the
// tag, header_size and a span of FieldKeys) plus a `lookup(tag)` helper.
// The Phase 6a lens consults that table — adding a resource is a pure
// schema change with zero edits to this header.
#include "IFE_FieldKeys.hpp"
#include "IFE_Resources.hpp"
#include "IFE_VTables.hpp"

namespace IFE {
namespace Reflective {

// =============================================================================
// Entry — one record per field in a fixed-layout resource block.
// =============================================================================
//
// Fields are sourced from the codegen `IFE::field_keys::<RES>::fields` arrays.
// `offset` is the field's *absolute* arena offset (block_offset + field_offset
// inside the block) so callers do not need to add the block offset themselves.
//
struct Entry {
    std::string_view name;        ///< Field name (string_view into a generated literal).
    IFE_FieldKind    kind;        ///< Element kind (SCALAR, BLOCK, ...).
    RECOVERY_TAG     scalar_tag;  ///< Per-element recovery tag (e.g. SCALAR_UINT64).
    std::uint64_t    offset;      ///< Absolute arena offset of the field's first byte.
    std::size_t      size;        ///< Field width in bytes.
};

// Inline-storage capacity for `Node::m_entries`. Derived at compile time
// from the codegen dispatch table so adding a resource with more fields
// automatically grows the storage — there is no hand-maintained constant
// to keep in sync with the schema.
inline constexpr std::size_t kMaxNodeEntries = []() noexcept {
    std::size_t m = 0;
    for (std::size_t i = 0; i < resources::table_size; ++i) {
        if (resources::table[i].field_count > m) {
            m = resources::table[i].field_count;
        }
    }
    return m;
}();

// =============================================================================
// Node — reflective view of a single block in an arena.
// =============================================================================
class Node {
public:
    /// Construct from an arena view + absolute offset of the block's first
    /// byte (the start of the universal preamble). Reads and validates the
    /// preamble; rejects offsets that don't carry a recognised resource tag
    /// or whose declared body extends past `view.capacity()`.
    ///
    /// Arrays (high bit set in the recovery tag) have variable bodies and are
    /// validated by `IFE::ArrayView<T>` rather than this constructor — they
    /// are accepted here so callers can iterate generic blocks, but the
    /// `entries()` span is empty for them; use `array<T>()` from a parent
    /// `Node` to walk array data instead.
    ///
    /// @throws std::invalid_argument on empty view, OOB, or a body that
    ///         doesn't fit within `view.capacity()`.
    Node(Memory::View view, std::uint64_t block_offset);

    /// Recovery tag from the universal preamble.
    RECOVERY_TAG recovery_tag() const noexcept { return m_tag; }

    /// 64-bit `validation` field from the universal preamble. Carries
    /// `IFE_FILE_MAGIC` for the FILE_HEADER block.
    std::uint64_t validation() const noexcept { return m_validation; }

    /// Absolute arena offset of the universal preamble's first byte.
    std::uint64_t block_offset() const noexcept { return m_block_offset; }

    /// Absolute arena offset of the block body (== block_offset + 10).
    std::uint64_t body_offset() const noexcept {
        return m_block_offset + DATA_BLOCK_HEADER_SIZE;
    }

    /// Hint at the body length in bytes. For fixed-layout resource blocks
    /// (per the codegen vtable) this is exact. For IFE_ARRAY blocks it is
    /// `count * step`. For unknown tags it is 0.
    std::size_t body_size_hint() const noexcept { return m_body_size_hint; }

    /// True iff this node carries a recognised fixed-layout resource tag
    /// (and therefore has a non-empty `entries()` span).
    bool has_fixed_layout() const noexcept { return !m_entries_view.empty(); }

    /// Reflective record for every field in the block. Empty for IFE_ARRAY
    /// blocks and any unknown tag.
    Span<const Entry> entries() const noexcept { return m_entries_view; }

    /// Number of fixed-layout entries (== entries().size()).
    std::size_t entry_count() const noexcept { return m_entries_view.size(); }

    /// Look up an entry by name. Returns nullptr if not found. The pointer
    /// is stable for the lifetime of this `Node`.
    const Entry* find_entry(std::string_view name) const noexcept {
        for (const auto& e : m_entries_view) {
            if (e.name == name) return &e;
        }
        return nullptr;
    }

    /// Read a single field by name as a `T`, with bounds + size validation.
    /// Always alignment-safe (memcpy load).
    ///
    /// @throws std::out_of_range  if `name` is not a field of this resource.
    /// @throws std::invalid_argument if `sizeof(T)` does not match the
    ///         field's recorded width.
    template <class T>
    T field(std::string_view name) const {
        static_assert(std::is_trivially_copyable_v<T>,
                      "Reflective::Node::field<T> requires trivially-copyable T");
        const Entry* e = find_entry(name);
        if (!e) {
            throw std::out_of_range(std::string{"IFE::Reflective::Node::field: "
                                                "unknown field '"} +
                                    std::string{name} + "'");
        }
        if (sizeof(T) != e->size) {
            throw std::invalid_argument(
                std::string{"IFE::Reflective::Node::field: size mismatch for '"} +
                std::string{name} + "' (entry=" + std::to_string(e->size) +
                ", T=" + std::to_string(sizeof(T)) + ")");
        }
        T tmp;
        std::memcpy(&tmp, m_view.data() + e->offset, sizeof(T));
        return tmp;
    }

    /// Convenience: read an 8-byte forward-pointer field by name. Equivalent
    /// to `field<std::uint64_t>(name)` and validated identically.
    std::uint64_t pointer(std::string_view name) const {
        return field<std::uint64_t>(name);
    }

    /// Follow the 8-byte forward-pointer field `name` to a child IFE_ARRAY
    /// block and return an `ArrayView<T>` over it.
    ///
    /// @throws std::out_of_range  if `name` is not a field, or if the
    ///         pointer is zero (NULL forward pointer / slot unset).
    /// @throws std::invalid_argument propagated from the `ArrayView<T>`
    ///         constructor (tag/kind/step mismatch, OOB).
    template <class T>
    ArrayView<T> array(std::string_view name) const {
        const std::uint64_t child_offset = pointer(name);
        if (child_offset == 0) {
            throw std::out_of_range(
                std::string{"IFE::Reflective::Node::array: NULL pointer for '"} +
                std::string{name} + "'");
        }
        return ArrayView<T>(m_view, child_offset);
    }

    /// Follow the 8-byte forward-pointer field `name` to a child block and
    /// return a fresh `Node` over it. Useful for walking the resource graph.
    Node child(std::string_view name) const {
        const std::uint64_t child_offset = pointer(name);
        if (child_offset == 0) {
            throw std::out_of_range(
                std::string{"IFE::Reflective::Node::child: NULL pointer for '"} +
                std::string{name} + "'");
        }
        return Node(m_view, child_offset);
    }

    /// Iterator support (range-for over entries()).
    const Entry* begin() const noexcept { return m_entries_view.begin(); }
    const Entry* end()   const noexcept { return m_entries_view.end(); }

private:
    /// Build the per-instance `m_entries` storage from the codegen
    /// `field_keys` array, rebasing each field's offset to absolute arena
    /// coordinates so callers don't need to add `block_offset` themselves.
    /// This is `O(N)` where N is the field count — at most ~11 for any
    /// resource in v1, so the construction cost is small enough to keep
    /// the lens stateless across calls (no caching needed).
    void build_entries(Span<const FieldKey> keys);

    Memory::View   m_view;            ///< Pinned arena reference.
    std::uint64_t  m_block_offset = 0;
    RECOVERY_TAG   m_tag           = RECOVERY_TAG::UNDEFINED;
    std::uint64_t  m_validation    = 0;
    std::size_t    m_body_size_hint = 0;

    /// Per-instance copies of the field-key catalog with absolute offsets.
    /// Capacity is `kMaxNodeEntries`, derived at compile time from the
    /// codegen dispatch table so adding a resource with more fields just
    /// grows the inline storage automatically.
    static constexpr std::size_t kMaxEntries = kMaxNodeEntries;
    Entry        m_entries[kMaxEntries]{};
    Span<const Entry> m_entries_view{};
};

// ---- Node implementation ---------------------------------------------------

inline void Node::build_entries(Span<const FieldKey> keys) {
    const std::size_t n = keys.size();
    if (n > kMaxEntries) {
        // The codegen table is the source of truth for the inline
        // capacity (`compute_max_entries()`), so this branch is reachable
        // only if a caller passes a hand-rolled FieldKey span that is
        // larger than any schema-defined resource. Surface as a bounds
        // error rather than silently truncating.
        throw std::invalid_argument(
            "IFE::Reflective::Node: span has more fields than "
            "kMaxEntries (codegen-derived from resources::table)");
    }
    for (std::size_t i = 0; i < n; ++i) {
        const FieldKey& fk = keys[i];
        m_entries[i].name       = fk.name;
        m_entries[i].kind       = fk.kind;
        m_entries[i].scalar_tag = fk.scalar_tag;
        m_entries[i].offset     = m_block_offset + fk.offset;
        m_entries[i].size       = fk.size;
    }
    m_entries_view = Span<const Entry>(m_entries, n);
}

inline Node::Node(Memory::View view, std::uint64_t block_offset)
    : m_view(std::move(view)), m_block_offset(block_offset) {
    if (!m_view) {
        throw std::invalid_argument("IFE::Reflective::Node: empty Memory::View");
    }
    const std::size_t cap = m_view.capacity();
    if (block_offset > cap || cap - block_offset < DATA_BLOCK_HEADER_SIZE) {
        throw std::invalid_argument(
            "IFE::Reflective::Node: preamble out of bounds");
    }
    const std::uint8_t* base = m_view.data() + block_offset;
    const auto h = read_header(base);
    m_tag        = h.tag();
    m_validation = h.validation;

    // Determine body extent.
    if ((h.recovery_tag & 0x8000u) != 0) {
        // IFE_ARRAY block: bounds-check the array header + body and record
        // the body size hint as count * step. We do *not* validate kind/step
        // against any specific TypeTraits<T> here — that's the job of
        // `ArrayView<T>` once a caller picks a concrete element type.
        if (cap - block_offset < IFE_ARRAY_HEADER_SIZE) {
            throw std::invalid_argument(
                "IFE::Reflective::Node: IFE_ARRAY header out of bounds");
        }
        std::uint16_t kas;
        std::memcpy(&kas, base + IFE_ARRAY_KIND_AND_STEP_OFFSET, sizeof(kas));
        std::uint32_t count;
        std::memcpy(&count, base + IFE_ARRAY_COUNT_OFFSET, sizeof(count));
        const auto unpacked = unpack_kind_and_step(kas);
        const std::size_t step  = unpacked.step_bytes;
        const std::size_t bytes = static_cast<std::size_t>(count) * step;
        if (cap - block_offset - IFE_ARRAY_HEADER_SIZE < bytes) {
            throw std::invalid_argument(
                "IFE::Reflective::Node: IFE_ARRAY body out of bounds");
        }
        m_body_size_hint = bytes;
        // Arrays have no fixed-layout entries; m_entries_view stays empty.
        return;
    }

    // Fixed-layout resource (or unknown tag): consult the codegen dispatch
    // table. A single linear scan over `IFE::resources::table` (one entry
    // per resource — at most a few dozen) replaces what used to be two
    // parallel N-way switches.
    const auto* info = resources::lookup(m_tag);
    if (info == nullptr) {
        // Unknown / non-resource tag (primitive scalar, BLOCK datatype, ...).
        // We accept it so callers can still observe `recovery_tag()` and
        // `validation()`, but body_size_hint and entries are empty.
        m_body_size_hint = 0;
        return;
    }
    const std::size_t body = info->header_size - DATA_BLOCK_HEADER_SIZE;
    if (cap - block_offset < DATA_BLOCK_HEADER_SIZE + body) {
        throw std::invalid_argument(
            "IFE::Reflective::Node: resource body out of bounds");
    }
    m_body_size_hint = body;
    build_entries(Span<const FieldKey>(info->fields, info->field_count));
}

}  // namespace Reflective
}  // namespace IFE

#endif  // IFE_Reflective_hpp
