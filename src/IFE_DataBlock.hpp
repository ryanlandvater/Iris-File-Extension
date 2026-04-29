/**
 * @file IFE_DataBlock.hpp
 * @brief Universal 10-byte DATA_BLOCK header for the IFE wire format.
 *
 * Every block in the IFE wire format leads with a uniform 10-byte preamble:
 *
 *     bytes [0, 8)  : `validation`   — uint64. For inline blocks it carries
 *                     the validation hash / sentinel; for the FILE_HEADER it
 *                     carries `IFE_FILE_MAGIC` so the very first eight
 *                     bytes of an `.iris` file identify the format.
 *     bytes [8,10)  : `recovery_tag` — uint16, an `IFE::RECOVERY_TAG` value
 *                     from `IFE_Types.hpp` (e.g. `RESOURCE_HEADER`,
 *                     `RESOURCE_TILE_TABLE`, ...).
 *
 * `IFE_FILE_MAGIC` packs the four ASCII bytes `'I','r','i','s'` into the
 * first 32 bits of the validation slot (zero-extended to 64 bits).
 *
 * This header is **dormant** unless built with `IFE_USE_FASTFHIR_SUBSTRATE`.
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#ifndef IFE_DataBlock_hpp
#define IFE_DataBlock_hpp

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "IFE_Types.hpp"
#include "IFE_Memory.hpp"

namespace IFE {

/// Wire size of the universal DATA_BLOCK preamble (8-byte validation +
/// 2-byte recovery tag).
inline constexpr std::size_t DATA_BLOCK_HEADER_SIZE = 10;

/// Byte offsets of the two fields inside the universal preamble.
inline constexpr std::size_t DATA_BLOCK_VALIDATION_OFFSET   = 0;
inline constexpr std::size_t DATA_BLOCK_RECOVERY_OFFSET     = 8;

/// File identification magic stored in the FILE_HEADER's `validation` slot.
/// 64-bit zero-extension of the four ASCII bytes `'I','r','i','s'` packed
/// little-endian (so a `LOAD_U32` at file offset 0 reads `0x49726973`).
inline constexpr std::uint64_t IFE_FILE_MAGIC =
    static_cast<std::uint64_t>(0x49726973u);

// Pin the universal-preamble size and the magic's intended bit layout at
// compile time so a future schema edit cannot silently break either.
static_assert(DATA_BLOCK_HEADER_SIZE ==
                  DATA_BLOCK_VALIDATION_OFFSET + sizeof(std::uint64_t) +
                  sizeof(std::uint16_t),
              "DATA_BLOCK_HEADER_SIZE inconsistent with field layout");
static_assert((IFE_FILE_MAGIC & 0xFFFFFFFFu) == 0x49726973u,
              "Low 32 bits of IFE_FILE_MAGIC must spell 'Iris'");

/// POD view of a universal block header. Returned by `read_header`.
struct DataBlockHeader {
    std::uint64_t validation   = 0;
    std::uint16_t recovery_tag = 0;  ///< raw underlying value of `IFE::RECOVERY_TAG`

    /// Convenience: typed view of the recovery tag.
    constexpr RECOVERY_TAG tag() const noexcept {
        return static_cast<RECOVERY_TAG>(recovery_tag);
    }
};

namespace detail {

inline std::uint64_t load_u64_le(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline std::uint16_t load_u16_le(const std::uint8_t* p) noexcept {
    std::uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline void store_u64_le(std::uint8_t* p, std::uint64_t v) noexcept {
    std::memcpy(p, &v, sizeof(v));
}

inline void store_u16_le(std::uint8_t* p, std::uint16_t v) noexcept {
    std::memcpy(p, &v, sizeof(v));
}

}  // namespace detail

/// Read the universal preamble at `buf[0..10)`.
inline DataBlockHeader read_header(const std::uint8_t* buf) noexcept {
    DataBlockHeader h;
    h.validation   = detail::load_u64_le(buf + DATA_BLOCK_VALIDATION_OFFSET);
    h.recovery_tag = detail::load_u16_le(buf + DATA_BLOCK_RECOVERY_OFFSET);
    return h;
}

/// Write the universal preamble at `buf[0..10)`.
inline void write_header(std::uint8_t* buf,
                         RECOVERY_TAG  tag,
                         std::uint64_t validation = 0) noexcept {
    detail::store_u64_le(buf + DATA_BLOCK_VALIDATION_OFFSET, validation);
    detail::store_u16_le(buf + DATA_BLOCK_RECOVERY_OFFSET,
                         static_cast<std::uint16_t>(tag));
}

/// Probe an arena byte at `offset` and return true iff it carries the
/// expected universal preamble. Bounds-checked against `view.capacity()`.
inline bool validate_at(const Memory::View& view,
                        std::uint64_t       offset,
                        RECOVERY_TAG        expected_tag) noexcept {
    if (!view) return false;
    if (offset > view.capacity()) return false;
    if (view.capacity() - offset < DATA_BLOCK_HEADER_SIZE) return false;
    const auto h = read_header(view.data() + offset);
    return h.tag() == expected_tag;
}

/// True iff `buf[0..8)` carries the file magic. Useful for very early
/// identification of an arena that has been populated with a FILE_HEADER.
inline bool is_file_magic(const std::uint8_t* buf) noexcept {
    return detail::load_u64_le(buf) == IFE_FILE_MAGIC;
}

}  // namespace IFE

#endif  // IFE_DataBlock_hpp
