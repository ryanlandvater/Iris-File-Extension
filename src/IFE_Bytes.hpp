/**
 * @file IFE_Bytes.hpp
 * @brief Scalar load/store primitives for the IFE byte stream.
 *
 * This is the only place in the generated layer where byte order and packed
 * widths appear. Everything above it — the generated block readers, writers
 * and validators — reaches the wire exclusively through these functions, so
 * the rules below are enforced once rather than at every field access.
 *
 * Successor to the LOAD_ and STORE_ family in src/IrisCodecExtension.cpp:125-177.
 * Coverage is the same; three defects in that family are deliberately not
 * carried forward, each noted at the function that replaces it.
 *
 * Invariants:
 *   - A load reads exactly the field's width and never a byte more.
 *   - Byte order is resolved at compile time; there is no runtime dispatch.
 *   - Every access is a memcpy. No pointer is ever reinterpreted and
 *     dereferenced, so unaligned fields — which IFE has, because packing is
 *     dense — are well defined rather than merely working in practice.
 *   - Nothing here allocates or throws.
 *
 * IFE stores little-endian on the wire at every version. A big-endian host
 * byte-swaps on load and store; that is the only reason the swap exists.
 */

#ifndef IFE_Bytes_hpp
#define IFE_Bytes_hpp

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

// Included directly rather than assumed from the includer: this header is
// self-contained by design.  src/IrisCodecExtension.hpp:77 says
// `using namespace Iris;` without including the headers that define those
// types, and relies on the .cpp having included them first — a fragility the
// generated layer does not inherit.
#include "IrisTypes.hpp"
#include "IrisCodecTypes.hpp"

namespace IFE {

using Iris::BYTE;
using IrisCodec::Offset;
using IrisCodec::Size;

/// Float and double are stored as IEEE 754 bit patterns, so the host's
/// representation must be the same one. v1 carried software conversion paths
/// for non-IEC559 hosts (IrisCodecExtension.cpp:196-235); no such host has
/// ever run this code, and carrying that machinery meant every float access
/// paid an indirect call. If a non-IEEE target ever appears this assertion is
/// the single place that has to change.
static_assert(std::numeric_limits<float>::is_iec559,
              "IFE requires IEEE 754 binary32 floats");
static_assert(std::numeric_limits<double>::is_iec559,
              "IFE requires IEEE 754 binary64 doubles");
static_assert(sizeof(float) == 4 && sizeof(double) == 8);

namespace detail {

/// Compile-time byte-order decision. v1 selected between little- and
/// big-endian readers through `static std::function` objects
/// (IrisCodecExtension.cpp:166-177): an indirect call for every field access
/// and a static-initialisation-order hazard for the price of a branch the
/// compiler can fold away.
inline constexpr bool swap_needed = (std::endian::native == std::endian::big);

static_assert(std::endian::native == std::endian::little ||
              std::endian::native == std::endian::big,
              "IFE does not support mixed-endian hosts");

/// std::byteswap is C++23; IFE targets C++20.
[[nodiscard]] constexpr std::uint16_t bswap(std::uint16_t v) noexcept {
    return static_cast<std::uint16_t>((v << 8) | (v >> 8));
}
[[nodiscard]] constexpr std::uint32_t bswap(std::uint32_t v) noexcept {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
[[nodiscard]] constexpr std::uint64_t bswap(std::uint64_t v) noexcept {
    return ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x00000000FF000000ull) << 8)  |
           ((v & 0x000000FF00000000ull) >> 8)  | ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
}
[[nodiscard]] constexpr std::uint8_t bswap(std::uint8_t v) noexcept { return v; }

/// Read exactly `N` bytes into the low end of a zeroed integer, correcting
/// byte order. `N` may be narrower than `sizeof(T)` — that is what makes the
/// packed widths safe.
template <typename T, std::size_t N = sizeof(T)>
[[nodiscard]] inline T load_bytes(const BYTE* __p) noexcept {
    static_assert(std::is_unsigned_v<T> && N <= sizeof(T));
    T raw{};
    if constexpr (swap_needed) {
        BYTE tmp[N];
        std::memcpy(tmp, __p, N);
        for (std::size_t i = 0; i < N / 2; ++i) {
            const BYTE t = tmp[i];
            tmp[i] = tmp[N - 1 - i];
            tmp[N - 1 - i] = t;
        }
        std::memcpy(&raw, tmp, N);
    } else {
        std::memcpy(&raw, __p, N);
    }
    return raw;
}

/// Write exactly `N` low-order bytes of `v`, correcting byte order.
template <typename T, std::size_t N = sizeof(T)>
inline void store_bytes(BYTE* __p, T v) noexcept {
    static_assert(std::is_unsigned_v<T> && N <= sizeof(T));
    if constexpr (swap_needed) {
        BYTE tmp[sizeof(T)];
        std::memcpy(tmp, &v, sizeof(T));
        for (std::size_t i = 0; i < N / 2; ++i) {
            const BYTE t = tmp[i];
            tmp[i] = tmp[N - 1 - i];
            tmp[N - 1 - i] = t;
        }
        std::memcpy(__p, tmp, N);
    } else {
        std::memcpy(__p, &v, N);
    }
}

}  // namespace detail

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
// MARK: - Whole-width scalars
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //

/// Load a scalar of the field's natural width. Integers are unsigned; `float`
/// and `double` are bit-cast from their byte-order-corrected representation.
template <typename T>
[[nodiscard]] inline T load(const BYTE* __p) noexcept {
    if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<float>(detail::load_bytes<std::uint32_t>(__p));
    } else if constexpr (std::is_same_v<T, double>) {
        return std::bit_cast<double>(detail::load_bytes<std::uint64_t>(__p));
    } else {
        static_assert(std::is_unsigned_v<T>, "IFE scalars are unsigned or IEEE floats");
        return detail::load_bytes<T>(__p);
    }
}

/// Store a scalar of the field's natural width.
template <typename T>
inline void store(BYTE* __p, T v) noexcept {
    if constexpr (std::is_same_v<T, float>) {
        detail::store_bytes<std::uint32_t>(__p, std::bit_cast<std::uint32_t>(v));
    } else if constexpr (std::is_same_v<T, double>) {
        detail::store_bytes<std::uint64_t>(__p, std::bit_cast<std::uint64_t>(v));
    } else {
        static_assert(std::is_unsigned_v<T>, "IFE scalars are unsigned or IEEE floats");
        detail::store_bytes<T>(__p, v);
    }
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
// MARK: - Packed widths (u24, u40)
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
// v1 read these by loading the next whole machine word and masking:
// `load_unaligned<uint32_t>(ptr) & U24_MASK` (IrisCodecExtension.cpp:132) and
// `load_unaligned<uint64_t>(ptr) & U40_MASK` (:128). Both over-read the field
// by three bytes. For the final TILE_OFFSETS entry of a file — a u40 offset
// followed by a u24 size, ending at the last byte — that is a read past the
// end of the mapping. These read exactly 3 and exactly 5.
//
// v1's big-endian u24 reader also masked with U40_MASK rather than U24_MASK
// (:133), returning two spurious bytes on any big-endian host. Not carried
// forward.

inline constexpr std::uint32_t U24_MAX = 0x00FFFFFFu;         ///< 16,777,215
inline constexpr std::uint64_t U40_MAX = 0x000000FFFFFFFFFFull;  ///< 1,099,511,627,775

/// Load a 24-bit field. Reads exactly three bytes.
[[nodiscard]] inline std::uint32_t load_u24(const BYTE* __p) noexcept {
    return detail::load_bytes<std::uint32_t, 3>(__p);
}

/// Store a 24-bit field. Writes exactly three bytes; `v` must be <= U24_MAX.
inline void store_u24(BYTE* __p, std::uint32_t v) noexcept {
    detail::store_bytes<std::uint32_t, 3>(__p, v & U24_MAX);
}

/// Load a 40-bit field. Reads exactly five bytes.
[[nodiscard]] inline std::uint64_t load_u40(const BYTE* __p) noexcept {
    return detail::load_bytes<std::uint64_t, 5>(__p);
}

/// Store a 40-bit field. Writes exactly five bytes; `v` must be <= U40_MAX.
inline void store_u40(BYTE* __p, std::uint64_t v) noexcept {
    detail::store_bytes<std::uint64_t, 5>(__p, v & U40_MAX);
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
// MARK: - Half precision (f16)
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
// IFE stores half floats on the wire (the associated-image ORIENTATION field
// is degrees as an IEEE binary16). C++20 has no half type, so these widen to
// `float`: half -> float is exact, and float -> half rounds to nearest, ties
// to even, as IEEE requires. The generated ORIENTATION_* constants are emitted
// as float literals so a caller can compare an accessor result against them
// directly.

/// Convert an IEEE binary16 bit pattern to `float`. Exact for every input,
/// including subnormals, infinities and NaN payloads.
[[nodiscard]] inline float half_to_float(std::uint16_t h) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    const std::uint32_t exp  = (h >> 10) & 0x1Fu;
    std::uint32_t       mant = h & 0x03FFu;

    if (exp == 0x1F) {  // infinity or NaN — payload preserved
        return std::bit_cast<float>(sign | 0x7F800000u | (mant << 13));
    }
    if (exp == 0) {
        if (mant == 0) return std::bit_cast<float>(sign);  // +/-0
        // Subnormal: shift until the implicit bit appears, charging each
        // shift to the exponent.
        std::uint32_t shift = 0;
        while ((mant & 0x0400u) == 0) { mant <<= 1; ++shift; }
        mant &= 0x03FFu;
        return std::bit_cast<float>(sign | ((113u - shift) << 23) | (mant << 13));
    }
    return std::bit_cast<float>(sign | ((exp + 112u) << 23) | (mant << 13));
}

/// Convert a `float` to an IEEE binary16 bit pattern, rounding to nearest with
/// ties to even. Values beyond half's range saturate to infinity.
[[nodiscard]] inline std::uint16_t float_to_half(float f) noexcept {
    const std::uint32_t x    = std::bit_cast<std::uint32_t>(f);
    const std::uint16_t sign = static_cast<std::uint16_t>((x >> 16) & 0x8000u);
    const std::int32_t  fexp = static_cast<std::int32_t>((x >> 23) & 0xFFu);
    const std::uint32_t fman = x & 0x007FFFFFu;

    if (fexp == 0xFF) {  // infinity or NaN — never round a NaN into infinity
        return static_cast<std::uint16_t>(sign | 0x7C00u | (fman ? 0x0200u : 0u));
    }

    const std::int32_t exp = fexp - 127 + 15;  // rebias to half
    if (exp >= 0x1F) return static_cast<std::uint16_t>(sign | 0x7C00u);  // overflow
    if (exp <= 0) {
        // Subnormal half, or too small to represent at all.
        if (exp < -10) return sign;
        const std::uint32_t m     = fman | 0x00800000u;   // restore implicit 1
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exp);
        std::uint32_t       h     = m >> shift;
        const std::uint32_t rem   = m & ((1u << shift) - 1u);
        const std::uint32_t midpoint = 1u << (shift - 1);
        if (rem > midpoint || (rem == midpoint && (h & 1u))) ++h;
        return static_cast<std::uint16_t>(sign | h);
    }
    std::uint32_t       h   = (static_cast<std::uint32_t>(exp) << 10) | (fman >> 13);
    const std::uint32_t rem = fman & 0x1FFFu;
    // A carry here propagates from mantissa into exponent, which is exactly
    // the right behaviour: 0x7BFF + 1 == 0x7C00 == infinity.
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return static_cast<std::uint16_t>(sign | h);
}

/// Load a 16-bit half-precision field, widened to `float`.
[[nodiscard]] inline float load_f16(const BYTE* __p) noexcept {
    return half_to_float(detail::load_bytes<std::uint16_t>(__p));
}

/// Store a `float` into a 16-bit half-precision field.
inline void store_f16(BYTE* __p, float v) noexcept {
    detail::store_bytes<std::uint16_t>(__p, float_to_half(v));
}

}  // namespace IFE

#endif  // IFE_Bytes_hpp
