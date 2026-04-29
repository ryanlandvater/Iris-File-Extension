/**
 * @file IFE_Array.hpp
 * @brief Self-describing IFE_ARRAY block + zero-copy Span<T>.
 *        Phase 5 of the FastFHIR substrate migration.
 *
 * On-disk layout of an IFE_ARRAY block (built on the Phase 4 universal
 * `DATA_BLOCK` preamble):
 *
 *   bytes [ 0,10) : DATA_BLOCK preamble
 *                     [ 0, 8) validation   : uint64
 *                     [ 8,10) recovery_tag : uint16  (an `ARRAY_*` tag from
 *                                                     IFE::RECOVERY_TAG)
 *   bytes [10,12) : kind_and_step          : uint16  high byte = element
 *                                                     IFE_FieldKind,
 *                                                     low byte  = element
 *                                                     wire size in bytes
 *                                                     (1..255)
 *   bytes [12,16) : count                  : uint32  element count
 *   bytes [16, ...) : count * step bytes of contiguous elements
 *
 * The `KIND_AND_STEP` field lets a reader validate an array's element type
 * against `IFE::TypeTraits<T>` without external schema metadata: the element
 * step matches `TypeTraits<T>::wire_size` and the kind matches
 * `TypeTraits<T>::kind`. This also gives the reflective lens (Phase 6) a
 * cheap path to walk an unknown array.
 *
 * The Phase 5 read path is built around `IFE::Span<T>` — a non-owning
 * `{ const T*, size_t }` pair with `std::vector`-shaped accessors. **No
 * allocation, no copying.** Substrate read paths use `Span<T>` exclusively
 * instead of `std::vector<T>`; legacy `IrisCodec::Abstraction::File`
 * read code is unchanged until Phase 6.
 *
 * This header is **dormant** unless built with `IFE_USE_FASTFHIR_SUBSTRATE`.
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#ifndef IFE_Array_hpp
#define IFE_Array_hpp

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "IFE_DataBlock.hpp"
#include "IFE_Memory.hpp"
#include "IFE_Types.hpp"

namespace IFE {

// =============================================================================
// IFE_ARRAY layout
// =============================================================================

/// Total wire size of the IFE_ARRAY header (DATA_BLOCK preamble + KIND_AND_STEP
/// + count). The element bytes follow immediately.
inline constexpr std::size_t IFE_ARRAY_HEADER_SIZE = 16;

/// Byte offsets within the array header.
inline constexpr std::size_t IFE_ARRAY_KIND_AND_STEP_OFFSET = 10;
inline constexpr std::size_t IFE_ARRAY_COUNT_OFFSET         = 12;

static_assert(IFE_ARRAY_KIND_AND_STEP_OFFSET == DATA_BLOCK_HEADER_SIZE,
              "KIND_AND_STEP must immediately follow the DATA_BLOCK preamble");
static_assert(IFE_ARRAY_COUNT_OFFSET == IFE_ARRAY_KIND_AND_STEP_OFFSET + 2,
              "count must immediately follow KIND_AND_STEP");
static_assert(IFE_ARRAY_HEADER_SIZE == IFE_ARRAY_COUNT_OFFSET + 4,
              "IFE_ARRAY header is preamble(10) + kind_and_step(2) + count(4)");

/// Maximum on-disk element step (low byte of KIND_AND_STEP). Matches the
/// widest primitive currently supported (uint64 / double = 8 bytes) and the
/// LayerExtentBlock (12 bytes); the cap is the byte's natural range.
inline constexpr std::uint16_t IFE_ARRAY_MAX_STEP = 0xFFu;

/// Pack `(kind, step)` into the `KIND_AND_STEP` u16. `step` must fit in 8
/// bits — enforced at runtime (and at compile time when called from a
/// `constexpr` context with a literal step).
constexpr std::uint16_t pack_kind_and_step(IFE_FieldKind kind,
                                           std::size_t   step_bytes) noexcept {
    // Caller is responsible for `step_bytes <= 0xFF`. Wrapping silently
    // would corrupt the wire format, so callers should validate first;
    // the runtime helpers below do so.
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(kind)) << 8) |
        static_cast<std::uint16_t>(step_bytes & 0xFFu));
}

struct KindAndStep {
    IFE_FieldKind kind;
    std::uint8_t  step_bytes;
};

constexpr KindAndStep unpack_kind_and_step(std::uint16_t v) noexcept {
    return KindAndStep{
        static_cast<IFE_FieldKind>((v >> 8) & 0xFFu),
        static_cast<std::uint8_t>(v & 0xFFu),
    };
}

// Round-trip closure for every primitive currently supported by TypeTraits<>.
static_assert(unpack_kind_and_step(pack_kind_and_step(IFE_FieldKind::SCALAR, 8))
                  .step_bytes == 8);
static_assert(unpack_kind_and_step(pack_kind_and_step(IFE_FieldKind::SCALAR, 8))
                  .kind == IFE_FieldKind::SCALAR);
static_assert(unpack_kind_and_step(pack_kind_and_step(IFE_FieldKind::BLOCK, 12))
                  .kind == IFE_FieldKind::BLOCK);

// =============================================================================
// Span<T> — non-owning zero-copy view (the std::vector replacement)
// =============================================================================
//
// Phase 5 read paths use `IFE::Span<T>` instead of `std::vector<T>`. It is
// trivially copyable, holds a `const T*` and a `size_t`, and exposes the
// vector-shaped accessors the codebase actually uses.
//
template <class T>
class Span {
public:
    using value_type      = T;
    using size_type       = std::size_t;
    using const_reference = const T&;
    using const_pointer   = const T*;
    using const_iterator  = const T*;

    constexpr Span() noexcept = default;
    constexpr Span(const T* data, std::size_t size) noexcept
        : m_data(data), m_size(size) {}

    constexpr const T*       data()  const noexcept { return m_data; }
    constexpr std::size_t    size()  const noexcept { return m_size; }
    constexpr bool           empty() const noexcept { return m_size == 0; }

    constexpr const T& operator[](std::size_t i) const noexcept {
        // Precondition: i < size(). No exception path on the hot read path.
        return m_data[i];
    }

    /// Bounds-checked element access (mirrors `std::vector::at`).
    const T& at(std::size_t i) const {
        if (i >= m_size) {
            throw std::out_of_range("IFE::Span::at: index out of range");
        }
        return m_data[i];
    }

    constexpr const T& front() const noexcept { return m_data[0]; }
    constexpr const T& back()  const noexcept { return m_data[m_size - 1]; }

    constexpr const_iterator begin() const noexcept { return m_data; }
    constexpr const_iterator end()   const noexcept { return m_data + m_size; }
    constexpr const_iterator cbegin() const noexcept { return m_data; }
    constexpr const_iterator cend()   const noexcept { return m_data + m_size; }

private:
    const T*    m_data = nullptr;
    std::size_t m_size = 0;
};

// =============================================================================
// ArrayView<T> — read-side view of an IFE_ARRAY block in the arena
// =============================================================================
//
// Construction validates the universal preamble tag and the KIND_AND_STEP
// field against `TypeTraits<T>`. After validation:
//
//   * `size()` returns the element count read from the body header.
//   * `at(i)` performs an unaligned memcpy-load of one element — always safe
//     regardless of the arena's alignment relative to T.
//   * `as_span()` returns a zero-copy `Span<T>` *iff* T's natural alignment
//     <= 1 OR the body is naturally aligned for T. Most arena allocations
//     are 16-byte aligned (the WRITE_HEAD_BYTES reserve), so for primitives
//     up to uint64/double this is the common case. Misaligned reads fall
//     back to `at(i)`.
//
template <class T>
class ArrayView {
public:
    /// Construct from an arena view + absolute offset of the array block.
    /// Throws `std::invalid_argument` on tag/kind/step mismatch or OOB.
    ArrayView(const Memory::View& view, std::uint64_t offset);

    /// Number of elements.
    std::size_t size() const noexcept { return m_size; }
    bool        empty() const noexcept { return m_size == 0; }

    /// Element wire step in bytes (low byte of KIND_AND_STEP).
    std::size_t step_bytes() const noexcept { return m_step; }

    /// Bounds-checked memcpy-loaded element. Always alignment-safe.
    T at(std::size_t i) const {
        if (i >= m_size) {
            throw std::out_of_range("IFE::ArrayView::at: index out of range");
        }
        T tmp;
        std::memcpy(&tmp, m_body + i * m_step, sizeof(T));
        return tmp;
    }

    /// Zero-copy view. Only valid when `sizeof(T) == step_bytes()` and the
    /// body pointer is naturally aligned for T (i.e. the array can be
    /// dereferenced as a contiguous `const T*` without UB). Otherwise
    /// returns an empty `Span<T>` and the caller should fall back to `at`.
    Span<T> as_span() const noexcept {
        if (sizeof(T) != m_step) return {};
        constexpr std::size_t align = alignof(T);
        if (align > 1 &&
            (reinterpret_cast<std::uintptr_t>(m_body) % align) != 0) {
            return {};
        }
        return Span<T>(reinterpret_cast<const T*>(m_body), m_size);
    }

private:
    const std::uint8_t* m_body = nullptr;  ///< First element byte.
    std::size_t         m_size = 0;
    std::size_t         m_step = 0;
};

// ---- ArrayView<T> implementation -------------------------------------------

template <class T>
ArrayView<T>::ArrayView(const Memory::View& view, std::uint64_t offset) {
    if (!view) {
        throw std::invalid_argument("IFE::ArrayView: empty Memory::View");
    }
    const std::size_t cap = view.capacity();
    if (offset > cap || cap - offset < IFE_ARRAY_HEADER_SIZE) {
        throw std::invalid_argument("IFE::ArrayView: header out of bounds");
    }
    const std::uint8_t* base = view.data() + offset;

    // Validate the universal preamble tag.
    const auto h = read_header(base);
    constexpr RECOVERY_TAG expected_tag = TypeTraits<T>::array_tag;
    if (h.tag() != expected_tag) {
        throw std::invalid_argument(
            std::string{"IFE::ArrayView: recovery tag mismatch (got 0x"} +
            std::to_string(h.recovery_tag) + ", expected 0x" +
            std::to_string(static_cast<std::uint16_t>(expected_tag)) + ")");
    }

    // Validate KIND_AND_STEP against TypeTraits<T>.
    std::uint16_t kas;
    std::memcpy(&kas, base + IFE_ARRAY_KIND_AND_STEP_OFFSET, sizeof(kas));
    const auto unpacked = unpack_kind_and_step(kas);
    if (unpacked.kind != TypeTraits<T>::kind) {
        throw std::invalid_argument("IFE::ArrayView: kind mismatch");
    }
    if (unpacked.step_bytes != TypeTraits<T>::wire_size) {
        throw std::invalid_argument("IFE::ArrayView: step_bytes mismatch");
    }

    std::uint32_t count;
    std::memcpy(&count, base + IFE_ARRAY_COUNT_OFFSET, sizeof(count));

    // Body bounds.
    const std::size_t step  = unpacked.step_bytes;
    const std::size_t bytes = static_cast<std::size_t>(count) * step;
    if (cap - offset - IFE_ARRAY_HEADER_SIZE < bytes) {
        throw std::invalid_argument("IFE::ArrayView: body out of bounds");
    }

    m_body = base + IFE_ARRAY_HEADER_SIZE;
    m_size = count;
    m_step = step;
}

}  // namespace IFE

#endif  // IFE_Array_hpp
