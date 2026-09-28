/**
 * @file IFE_Primitives.hpp
 * @brief The primitive block shapes every spec block instantiates.
 * @copyright Iris Developers, 2025-2026
 *
 * The specification declares five primitives (ife_fields.json §primitives),
 * and each block in its inventory instantiates exactly one:
 *
 *   FILE_HEADER — the root at byte 0: MAGIC + RECOVERY, no VALIDATION
 *   BLOCK       — the universal header: VALIDATION (u64, own offset) +
 *                 RECOVERY (u16 tag sharing the 0x55 high byte)
 *   ARRAY       — BLOCK + STRIDE (u16) + COUNT (u32): fixed-width entries
 *   BYTE_ARRAY  — BLOCK + COUNT (u32): an opaque byte run
 *   TILE_FRAME  — a prefix laid out BACKWARD from its stream's first byte,
 *                 identified by a forty-bit self-offset VALIDATION
 *
 * The generator derives each concrete block's shape — which primitive it
 * instantiates, its versioned header size, its extent — from the spec JSON
 * into generated_source/IFE_Blocks.hpp. This header is the behavior those
 * shapes share (the FF_Primitives.hpp equivalent) plus the reader-policy
 * helpers both layers need — root_at/versioned_root (the version bootstrap)
 * and note (map recording) — moved here when src/IFE_Common.hpp was deleted
 * (2026-08-28). Those helpers reference the generated FILE_HEADER and the
 * semantic FileMap, so this header includes the generated block layer and
 * the semantic layer; it is still not part of the public include chain.
 *
 * INCLUDE DISCIPLINE — this header is deliberately NOT included by
 * IrisFileExtension.hpp (or IFE_Blocks.hpp), so a consumer of the top-level
 * API never drags the byte-level machinery in transitively. It is included
 * only by the translation units that do the offset math and the reading:
 * the generated IFE_Blocks.cpp, the recovery engine, and the runtime.
 *
 * Invariants (same rules as IFE_Bytes.hpp):
 *   - Nothing here allocates or throws.
 *   - Every read is bounds-checked by the caller's size argument; the fit
 *     tests are sentinel-free — a NULL_OFFSET (all-ones) fails `off <= size`.
 */

#ifndef IFE_Primitives_hpp
#define IFE_Primitives_hpp

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "IFE_Bytes.hpp"
// The reader-policy helpers at the foot of this header construct the
// generated FILE_HEADER and record into the semantic FileMap, so this header
// reaches both layers. The discipline that matters — never being included by
// IrisFileExtension.hpp — is unchanged.
#include "IFE_Blocks.hpp"
#include "IrisFileExtension.hpp"
// The ADVANCED tier: `note()` below records into Abstraction::FileMap, so this
// internal header depends on the advanced tier's value types. The edge is
// deliberate — the primitives layer already reaches the semantic layer above.
#include "IFE_Advanced.hpp"

namespace Iris::File {
namespace primitives {

using Iris::BYTE;
using IrisCodec::Offset;
using IrisCodec::Size;

/// The high byte every recovery tag shares (ife_spec.adoc §ife-recovery-codes).
/// A scan tests `(tag >> 8) == this` before trusting a 0x55xx word, which is
/// what keeps its false-positive rate negligible.
inline constexpr std::uint8_t RECOVERY_TAG_PREFIX = 0x55;

/// Compose the file's version the way v1 does: major << 16 | minor. Read once
/// at the root and propagated to every block by construction.
[[nodiscard]] inline std::uint32_t compose_version(
    std::uint16_t __major, std::uint16_t __minor) noexcept {
    return (static_cast<std::uint32_t>(__major) << 16) | __minor;
}

/// The BLOCK primitive: the universal header every block reached through an
/// offset field opens with. VALIDATION stores the block's own byte offset, so
/// a reader that arrived by an offset can confirm read(x) == x before trusting
/// any content; the recovery tag then confirms the block type.
struct BlockHeader {
    static constexpr std::size_t VALIDATION = 0;  ///< u64: the block's own offset
    static constexpr std::size_t RECOVERY   = 8;  ///< u16: 0x55xx tag
    static constexpr std::size_t HEADER_SIZE = 10;

    [[nodiscard]] static constexpr bool fits(Offset __off, Size __size,
                                             Size __header_size) noexcept {
        return __off != 0 && __off <= __size && __size - __off >= __header_size;
    }

    [[nodiscard]] static constexpr Size extent(Size __header_size) noexcept {
        return __header_size;
    }

    [[nodiscard]] static std::uint64_t validation_at(const BYTE* __base, Offset __off) noexcept {
        return ::Iris::File::load<std::uint64_t>(__base + __off + VALIDATION);
    }

    [[nodiscard]] static std::uint16_t recovery_at(const BYTE* __base, Offset __off) noexcept {
        return ::Iris::File::load<std::uint16_t>(__base + __off + RECOVERY);
    }

    /// The scan's block signature: VALIDATION holds the block's own offset.
    /// The tag test is the caller's — entry_for() is the derived vocabulary.
    [[nodiscard]] static bool signature_matches(const BYTE* __base, Offset __off,
                                                Size __size) noexcept {
        return fits(__off, __size, HEADER_SIZE)
            && validation_at(__base, __off) == static_cast<std::uint64_t>(__off);
    }

    /// Both witnesses, cross-checked — the P0-3 reconciliation's raw material.
    /// validate_file_structure reads the same facts and uses them only to
    /// reject; this is the form that says *which* witness failed.
    enum class Witness : std::uint8_t { OK, OUT_OF_BOUNDS, BAD_VALIDATION, BAD_RECOVERY };

    [[nodiscard]] static Witness validate_offset(const BYTE* __base, Offset __off,
                                                 Size __size, std::uint16_t __expected_tag) noexcept {
        if (!fits(__off, __size, HEADER_SIZE)) return Witness::OUT_OF_BOUNDS;
        if (validation_at(__base, __off) != static_cast<std::uint64_t>(__off))
            return Witness::BAD_VALIDATION;
        if (recovery_at(__base, __off) != __expected_tag) return Witness::BAD_RECOVERY;
        return Witness::OK;
    }
};

/// The ARRAY primitive: BLOCK + a fixed-width entry run. STRIDE records the
/// entry width at encoding time, so a decoder steps by the writer's stride
/// rather than the size this build was compiled with — the forward-compatible
/// mechanism.
struct ArrayHeader {
    static constexpr std::size_t STRIDE = 10;  ///< u16: entry width at encoding time
    static constexpr std::size_t COUNT  = 12;  ///< u32: entry count
    static constexpr std::size_t HEADER_SIZE = 16;

    [[nodiscard]] static constexpr Offset entries_at(Offset __off, Size __header_size) noexcept {
        return __off + __header_size;
    }

    [[nodiscard]] static constexpr Size extent(Size __header_size, Size __stride,
                                               Size __count) noexcept {
        return __header_size + __stride * __count;
    }

    [[nodiscard]] static constexpr bool fits(Offset __off, Size __size, Size __header_size,
                                             Size __stride, Size __count) noexcept {
        return BlockHeader::fits(__off, __size, __header_size)
            && __stride * __count <= __size - (__off + __header_size);
    }

    /// How many entries can be READ without leaving the file: the stamped
    /// COUNT when the file holds them all, otherwise every entry whose full
    /// `__entry_size` bytes lie inside it. For walkers over files that may be
    /// damaged, which must still see a damaged array's surviving entries --
    /// bound by GEOMETRY, never by "stop at the first bad entry" -- and must
    /// never read past EOF to do it. A zero stride stacks every entry on one
    /// slot, which is no array at all: none.
    ///
    /// `__entry_size` is the widest entry this build reads. Against an older
    /// file whose narrower stride puts its last entry flush at EOF that is
    /// one entry conservative; no entry read through here has an appended
    /// field today, so it is exact.
    [[nodiscard]] static constexpr std::uint32_t readable(Offset __entries, Size __size,
                                                          Size __stride, std::uint32_t __count,
                                                          Size __entry_size) noexcept {
        if (__stride == 0 || __entries > __size || __size - __entries < __entry_size) return 0;
        const Size fit = (__size - __entries - __entry_size) / __stride + 1;
        return fit < __count ? static_cast<std::uint32_t>(fit) : __count;
    }
};

/// The BYTE_ARRAY primitive: BLOCK + an opaque run. It stores no STRIDE: the
/// stride of a byte array is intrinsically one, so COUNT is a byte count
/// exclusive of the header.
struct ByteArrayHeader {
    static constexpr std::size_t COUNT = 10;  ///< u32: byte count, exclusive of the header
    static constexpr std::size_t HEADER_SIZE = 14;

    [[nodiscard]] static constexpr Size extent(Size __header_size, Size __count) noexcept {
        return __header_size + __count;
    }

    [[nodiscard]] static constexpr bool fits(Offset __off, Size __size, Size __header_size,
                                             Size __count) noexcept {
        return BlockHeader::fits(__off, __size, __header_size)
            && __count <= __size - (__off + __header_size);
    }
};

/// The FILE_HEADER primitive: the root at byte 0, the one block reached
/// without an offset. MAGIC occupies the slot VALIDATION would — at byte 0
/// that field could only ever store zero — so a header can never pass the
/// self-offset test; identification is MAGIC + recovery tag, and nothing else.
struct RootHeader {
    static constexpr std::size_t MAGIC = 0;   ///< u32: 'Iris' in little-endian
    static constexpr std::size_t RECOVERY = 4;  ///< u16: RECOVER_FILE_HEADER
    /// The version-invariant identification prefix: the six bytes every IFE
    /// file opens with, and all a reader may check before parsing anything.
    static constexpr std::size_t IDENTIFIER_SIZE = 6;

    [[nodiscard]] static constexpr bool fits(Offset __off, Size __size) noexcept {
        return __off <= __size && __size - __off >= IDENTIFIER_SIZE;
    }

    [[nodiscard]] static bool magic_matches(const BYTE* __base, Offset __off,
                                            std::uint32_t __magic) noexcept {
        return ::Iris::File::load<std::uint32_t>(__base + __off + MAGIC) == __magic;
    }

    [[nodiscard]] static bool identifies(const BYTE* __base, Offset __off, Size __size,
                                         std::uint32_t __magic, std::uint16_t __root_tag) noexcept {
        return fits(__off, __size) && magic_matches(__base, __off, __magic)
            && ::Iris::File::load<std::uint16_t>(__base + __off + RECOVERY) == __root_tag;
    }
};

/// The TILE_FRAME primitive: a prefix laid out backward from its stream's
/// first byte. Its VALIDATION is forty bits — which no other structure in the
/// format writes — so the u40 self-offset test alone identifies a frame,
/// needing no recovery tag. The frame's total width is per-instance (the
/// generated TILE_PIXEL_DATA header size, spec-derived); only the shape rules
/// live here.
struct FrameHeader {
    static constexpr std::size_t VALIDATION_SIZE = 5;  ///< u40

    /// VALIDATION sits five bytes before the anchor and stores its own
    /// position, not the anchor's.
    [[nodiscard]] static std::uint64_t validation_at(const BYTE* __base, Offset __anchor) noexcept {
        return ::Iris::File::load_u40(__base + __anchor - VALIDATION_SIZE);
    }

    [[nodiscard]] static bool signature_matches(const BYTE* __base, Offset __anchor,
                                                Size __size) noexcept {
        return __anchor >= VALIDATION_SIZE && __anchor <= __size
            && validation_at(__base, __anchor)
                   == static_cast<std::uint64_t>(__anchor - VALIDATION_SIZE);
    }

    [[nodiscard]] static constexpr Size extent(Size __header_size) noexcept {
        return __header_size;
    }
};

}  // namespace primitives
}  // namespace Iris::File

// ---------------------------------------------------------------------------
// Reader policy (moved from the deleted src/IFE_Common.hpp, 2026-08-28): the
// version bootstrap and the map-recording convention shared by the runtime
// and the recovery engine. These reference the generated block layer and the
// semantic layer, and sit at IFE scope because every call site does.
// ---------------------------------------------------------------------------
namespace Iris::File {

/// The root handle. Its own version is unknowable until it has been read, so
/// it is constructed with UINT32_MAX — every gate open for exactly one block —
/// which lets it read its own version field.
inline ::Iris::File::blocks::FILE_HEADER root_at(const BYTE* __base, size_t __size) noexcept {
    return ::Iris::File::blocks::FILE_HEADER{__base, 0, __size, UINT32_MAX};
}

inline ::Iris::File::blocks::FILE_HEADER versioned_root(const BYTE* __base, size_t __size) noexcept {
    const ::Iris::File::blocks::FILE_HEADER bootstrap = root_at(__base, __size);
    // A root that does not fit its header must not have its version fields
    // read — the accessors are raw loads, and a tiny buffer would read past
    // the mapping (caught by the tiny-file scan test under ASan). The
    // VERSION_WRITTEN fallback mirrors declared_version's; the caller's
    // validate() gates whatever happens next.
    if (!bootstrap)
        return ::Iris::File::blocks::FILE_HEADER{__base, 0, __size,
                                          ::Iris::File::blocks::VERSION_WRITTEN};
    return ::Iris::File::blocks::FILE_HEADER{__base, 0, __size,
                                      ::Iris::File::primitives::compose_version(
                                          bootstrap.extension_major(),
                                          bootstrap.extension_minor())};
}

/// ArrayHeader::readable() for a generated array handle: how many of its
/// entries may be read without leaving the file. Every walker over a file that
/// may be damaged bounds its loop with this — never with the stamped COUNT (a
/// claim), and never with `(size - begin) / stride`, which lets a damaged stride
/// narrower than the entry carry the last entry's fields past EOF. Both
/// mistakes read past the mapping under ASan: the map walk the first time the
/// sweep ran it, the recovery enumerators on 2026-09-28.
template <class Array>
[[nodiscard]] std::uint32_t readable_entries(const Array& __a) noexcept {
    using Entry = std::remove_cvref_t<decltype(__a.entry(0))>;
    return ::Iris::File::primitives::ArrayHeader::readable(__a.entries_begin(), __a.__size,
                                                    __a.stride(), __a.count(),
                                                    Entry::entry_size);
}

/// Why the stream at @p __anchor, on a Z-stacked layer, is not properly framed
/// — or nullptr when it is. Every stream of a layer whose Z_PLANES exceeds one
/// is framed (the spec, Focal Planes): the frame is the only place the format
/// records how many planes a tile carries. The frame must name the stream's own
/// global tile index and claim no more planes than its layer.
///
/// Presence is tested explicitly: `validate()` passes vacuously when the
/// handle's version predates the frame (its VALIDATION accessor is gated
/// empty), so a bare `validate()` would call every stream framed. The caller
/// has already bounds-checked the entry, so `__anchor` lies past the header.
inline const char* tile_frame_error(const BYTE* __base, Size __size, std::uint32_t __version,
                                    Offset __anchor, std::uint64_t __global,
                                    std::uint16_t __layer_planes) noexcept {
    const ::Iris::File::blocks::TILE_PIXEL_DATA frame{__base, __anchor, __size, __version};
    if (!frame.validation() || !frame.validate())
        return "is on a Z-stacked layer but has no tile frame";
    if (frame.tile_index().value_or(0) != __global)
        return "has a tile frame naming a different tile";
    if (frame.z_planes().value_or(0) > __layer_planes)
        return "has a tile frame claiming more planes than its layer holds";
    return nullptr;
}

/// Record one block, keyed by offset. `size` is what the block occupies,
/// header and payload together.
inline void note(Abstraction::FileMap& __map, Abstraction::MapEntryType __type,
                 Offset __offset, Size __size) {
    __map[__offset] = {.type = __type, .offset = __offset, .size = __size};
}

}  // namespace Iris::File
#endif  // IFE_Primitives_hpp
