/**
 * @file ife_bytes_tests.cpp
 * @brief Unit tests for the IFE_Bytes scalar primitives.
 *
 * Self-contained (no external test framework) so CI needs no extra deps.
 * Run with `ctest` or directly; non-zero exit on failure.
 *
 * Two properties get exhaustive rather than sampled coverage, because both
 * are places where "works on the values I thought of" is not good enough:
 *   - the packed widths must touch exactly 3 and exactly 5 bytes, which is
 *     checked with guard bytes rather than by inspection;
 *   - half precision is only 65,536 values, so every one is tested.
 */
#include "IFE_Bytes.hpp"

// The generated constants and these primitives are two halves of one
// decision: the spec stores half floats as wire bit patterns, the generator
// decodes them to float literals, and load_f16 widens the wire value to
// float. If either half changes alone, a comparison against ORIENTATION_*
// silently stops matching. The cross-check below is what makes that loud.
#include "IFE_Blocks.hpp"

#include <cmath>
#include <memory>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

using IFE::BYTE;

// A buffer with poison either side of the field, so an over-read or
// over-write is detected rather than tolerated.
struct Guarded {
    static constexpr std::size_t PAD = 8;
    BYTE raw[PAD * 2 + 16];

    explicit Guarded(std::size_t field_size) : raw{}, size(field_size) {
        std::memset(raw, 0xA5, sizeof(raw));
    }
    BYTE*       field()       { return raw + PAD; }
    const BYTE* field() const { return raw + PAD; }
    bool guards_intact() const {
        for (std::size_t i = 0; i < PAD; ++i)
            if (raw[i] != 0xA5) return false;
        for (std::size_t i = PAD + size; i < sizeof(raw); ++i)
            if (raw[i] != 0xA5) return false;
        return true;
    }
    std::size_t size;
};

template <typename T>
void roundtrip(T value) {
    Guarded g(sizeof(T));
    IFE::store<T>(g.field(), value);
    IFE_CHECK(IFE::load<T>(g.field()) == value);
    IFE_CHECK(g.guards_intact());
}

void test_whole_width_scalars() {
    for (std::uint8_t v : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{0x7F},
                           std::uint8_t{0xFE}, std::uint8_t{0xFF}})
        roundtrip<std::uint8_t>(v);
    for (std::uint16_t v : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x00FF},
                            std::uint16_t{0xFFFE}, std::uint16_t{0xFFFF}})
        roundtrip<std::uint16_t>(v);
    for (std::uint32_t v : {0u, 1u, 0x0000FFFFu, 0x12345678u, 0xFFFFFFFEu, 0xFFFFFFFFu})
        roundtrip<std::uint32_t>(v);
    for (std::uint64_t v : {0ull, 1ull, 0xFFFFFFFFull, 0x0123456789ABCDEFull,
                            0xFFFFFFFFFFFFFFFEull, 0xFFFFFFFFFFFFFFFFull})
        roundtrip<std::uint64_t>(v);
    for (float v : {0.0f, -0.0f, 1.0f, -1.0f, 3.14159265f, 1e-30f, 1e30f})
        roundtrip<float>(v);
    for (double v : {0.0, -0.0, 1.0, -1.0, 3.141592653589793, 1e-300, 1e300})
        roundtrip<double>(v);
}

// Every byte offset within a word, to prove nothing depends on alignment.
// IFE packs densely, so unaligned fields are the norm: FILE_SIZE sits at
// offset 6 of the file header.
void test_unaligned() {
    BYTE buf[64];
    for (std::size_t off = 0; off < 8; ++off) {
        std::memset(buf, 0, sizeof(buf));
        IFE::store<std::uint64_t>(buf + off, 0x0123456789ABCDEFull);
        IFE_CHECK(IFE::load<std::uint64_t>(buf + off) == 0x0123456789ABCDEFull);
        IFE::store<std::uint32_t>(buf + off, 0xDEADBEEFu);
        IFE_CHECK(IFE::load<std::uint32_t>(buf + off) == 0xDEADBEEFu);
        IFE::store<float>(buf + off, 2.5f);
        IFE_CHECK(IFE::load<float>(buf + off) == 2.5f);
    }
}

// The defect this header exists to fix: v1 read a u24 by loading four bytes
// and masking, and a u40 by loading eight. Guard bytes make an over-read
// visible; a store that wrote a fourth or sixth byte would trip them too.
void test_packed_widths_touch_exact_bytes() {
    for (std::uint32_t v : {0u, 1u, 0x00FFFFFEu, IFE::U24_MAX}) {
        Guarded g(3);
        IFE::store_u24(g.field(), v);
        IFE_CHECK(IFE::load_u24(g.field()) == v);
        IFE_CHECK(g.guards_intact());
    }
    // Named array rather than a braced list: uint64_t is `unsigned long` on
    // LP64 targets such as s390x and `unsigned long long` elsewhere, so mixing
    // a `ull` literal with U40_MAX in one initializer_list fails to deduce
    // there while compiling cleanly here.
    constexpr std::uint64_t u40_cases[] = {0, 1, 0x000000FFFFFFFFFEull, IFE::U40_MAX};
    for (std::uint64_t v : u40_cases) {
        Guarded g(5);
        IFE::store_u40(g.field(), v);
        IFE_CHECK(IFE::load_u40(g.field()) == v);
        IFE_CHECK(g.guards_intact());
    }
    // A u24 load must not see the byte after the field, whatever it holds.
    BYTE buf[4] = {0x11, 0x22, 0x33, 0xFF};
    IFE_CHECK((IFE::load_u24(buf) & ~IFE::U24_MAX) == 0);
    IFE_CHECK(IFE::load_u24(buf) <= IFE::U24_MAX);
    // Likewise a u40 must not see bytes 5..7.
    BYTE buf40[8] = {1, 2, 3, 4, 5, 0xFF, 0xFF, 0xFF};
    IFE_CHECK(IFE::load_u40(buf40) <= IFE::U40_MAX);
}

// Guard bytes prove nothing was WRITTEN out of bounds; they cannot prove
// nothing was READ, because an over-read lands inside the same buffer and is
// then masked away invisibly. That is exactly how v1's defect survived.
//
// Detecting it needs the field to end where the allocation ends, so a fourth
// or sixth byte is genuinely not ours. Under AddressSanitizer that is a
// heap-buffer-overflow and the process aborts naming this function; without a
// sanitizer these are ordinary passing round-trips. The CI job that builds
// with -fsanitize=address is what makes this test meaningful.
void test_packed_loads_do_not_over_read() {
    {
        auto exact = std::make_unique<BYTE[]>(3);   // three bytes, no more
        exact[0] = 0x78; exact[1] = 0x56; exact[2] = 0x34;
        IFE_CHECK(IFE::load_u24(exact.get()) <= IFE::U24_MAX);
    }
    {
        auto exact = std::make_unique<BYTE[]>(5);   // five bytes, no more
        for (int i = 0; i < 5; ++i) exact[i] = static_cast<BYTE>(i + 1);
        IFE_CHECK(IFE::load_u40(exact.get()) <= IFE::U40_MAX);
    }
    {   // The real shape: a TILE_OFFSETS entry ending at the last byte of a file.
        auto file_end = std::make_unique<BYTE[]>(8);
        for (int i = 0; i < 8; ++i) file_end[i] = static_cast<BYTE>(0xF0 + i);
        IFE_CHECK(IFE::load_u40(file_end.get())     <= IFE::U40_MAX);
        IFE_CHECK(IFE::load_u24(file_end.get() + 5) <= IFE::U24_MAX);
    }
}

// The wire contract, stated as literal bytes.
//
// Every other test in this file round-trips store through load, which cancels
// a byte-order error exactly when there is one to catch: if both halves agree
// on the wrong order, the round-trip still passes. This is the only test that
// says what a byte on disk must actually be, in both directions, and it is
// what pins "little endian on disk, at every version" rather than merely
// "self-consistent on this host".
//
// It caught nothing on a little-endian host by luck: the primitives compose
// values arithmetically and so have no host-dependent path left. Before that,
// the big-endian branch stored a u24 as `BB AA 00` and loaded it back shifted
// left by 8 — invisible to a round-trip test on the only hosts available.
void test_wire_byte_order() {
    BYTE buf[8];

    // Whole widths.
    std::memset(buf, 0, sizeof(buf));
    IFE::store<std::uint16_t>(buf, 0xAABB);
    IFE_CHECK(buf[0] == 0xBB && buf[1] == 0xAA);

    std::memset(buf, 0, sizeof(buf));
    IFE::store<std::uint32_t>(buf, 0xAABBCCDDu);
    IFE_CHECK(buf[0] == 0xDD && buf[1] == 0xCC && buf[2] == 0xBB && buf[3] == 0xAA);

    std::memset(buf, 0, sizeof(buf));
    IFE::store<std::uint64_t>(buf, 0x1122334455667788ull);
    IFE_CHECK(buf[0] == 0x88 && buf[1] == 0x77 && buf[2] == 0x66 && buf[3] == 0x55 &&
              buf[4] == 0x44 && buf[5] == 0x33 && buf[6] == 0x22 && buf[7] == 0x11);

    // Packed widths — the pair that was wrong, and the reason this test exists.
    std::memset(buf, 0, sizeof(buf));
    IFE::store_u24(buf, 0xAABBCCu);
    IFE_CHECK(buf[0] == 0xCC && buf[1] == 0xBB && buf[2] == 0xAA);

    std::memset(buf, 0, sizeof(buf));
    IFE::store_u40(buf, 0xAABBCCDDEEull);
    IFE_CHECK(buf[0] == 0xEE && buf[1] == 0xDD && buf[2] == 0xCC &&
              buf[3] == 0xBB && buf[4] == 0xAA);

    // float is an IEEE binary32 bit pattern in the same order: 2.5f is
    // 0x40200000, so the wire reads 00 00 20 40.
    std::memset(buf, 0, sizeof(buf));
    IFE::store<float>(buf, 2.5f);
    IFE_CHECK(buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x20 && buf[3] == 0x40);

    // f16 likewise: ORIENTATION_90 is the wire pattern 0x55A0.
    std::memset(buf, 0, sizeof(buf));
    IFE::store_f16(buf, 90.0f);
    IFE_CHECK(buf[0] == 0xA0 && buf[1] == 0x55);

    // The load direction, from bytes this test wrote by hand rather than
    // through the store it is checking.
    const BYTE u16_wire[2] = {0xBB, 0xAA};
    const BYTE u24_wire[3] = {0xCC, 0xBB, 0xAA};
    const BYTE u32_wire[4] = {0xDD, 0xCC, 0xBB, 0xAA};
    const BYTE u40_wire[5] = {0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    const BYTE u64_wire[8] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    const BYTE f16_wire[2] = {0xA0, 0x55};
    IFE_CHECK(IFE::load<std::uint16_t>(u16_wire) == 0xAABB);
    IFE_CHECK(IFE::load_u24(u24_wire)            == 0xAABBCCu);
    IFE_CHECK(IFE::load<std::uint32_t>(u32_wire) == 0xAABBCCDDu);
    IFE_CHECK(IFE::load_u40(u40_wire)            == 0xAABBCCDDEEull);
    IFE_CHECK(IFE::load<std::uint64_t>(u64_wire) == 0x1122334455667788ull);
    IFE_CHECK(IFE::load_f16(f16_wire)            == 90.0f);
}

// The tile table is where the packed widths actually live: a u40 offset
// followed by a u24 size, 8 bytes per entry.
void test_tile_offset_entry_shape() {
    Guarded g(8);
    const std::uint64_t offset = 0x000000FEDCBA9876ull;
    const std::uint32_t size   = 0x00ABCDEFu & IFE::U24_MAX;
    IFE::store_u40(g.field(), offset);
    IFE::store_u24(g.field() + 5, size);
    IFE_CHECK(IFE::load_u40(g.field()) == offset);
    IFE_CHECK(IFE::load_u24(g.field() + 5) == size);
    IFE_CHECK(g.guards_intact());
}

// All 65,536 half patterns: widening is exact, and re-narrowing returns the
// original bits. Catches sign, subnormal, infinity and NaN handling in one
// sweep rather than at the values someone happened to pick.
void test_half_exhaustive() {
    int mismatches = 0;
    for (std::uint32_t bits = 0; bits <= 0xFFFFu; ++bits) {
        const auto  h = static_cast<std::uint16_t>(bits);
        const float f = IFE::half_to_float(h);
        const std::uint16_t back = IFE::float_to_half(f);

        const bool is_nan = ((h >> 10) & 0x1F) == 0x1F && (h & 0x3FF) != 0;
        if (is_nan) {
            IFE_CHECK(std::isnan(f));
            if (!std::isnan(f)) ++mismatches;
            continue;
        }
        if (back != h && ++mismatches <= 5)
            std::fprintf(stderr, "FAIL: half 0x%04X -> %g -> 0x%04X\n", h, static_cast<double>(f), back);
    }
    IFE_CHECK(mismatches == 0);

    Guarded g(2);
    IFE::store_f16(g.field(), 90.0f);
    IFE_CHECK(IFE::load_f16(g.field()) == 90.0f);
    IFE_CHECK(g.guards_intact());
}

// The values the format actually carries, checked against the bit patterns
// committed in spec/ife_constants.json.
void test_orientation_constants() {
    struct { std::uint16_t bits; float degrees; } cases[] = {
        {0x0000,    0.0f}, {0x55A0,   90.0f}, {0x59A0,  180.0f}, {0x5C38,  270.0f},
        {0xD5A0,  -90.0f}, {0xD9A0, -180.0f}, {0xDC38, -270.0f},
    };
    for (const auto& c : cases) {
        IFE_CHECK(IFE::half_to_float(c.bits) == c.degrees);
        IFE_CHECK(IFE::float_to_half(c.degrees) == c.bits);
    }

    // Read a wire value through the primitive and compare it against the
    // generated constant, as a caller would.
    namespace k = IFE::constants;
    BYTE wire[2];
    IFE::store_f16(wire, 90.0f);
    IFE_CHECK(IFE::load_f16(wire) == k::ORIENTATION_90);
    IFE::store_f16(wire, -270.0f);
    IFE_CHECK(IFE::load_f16(wire) == k::ORIENTATION_MINUS_270);

    IFE_CHECK(k::ORIENTATION_0        ==    0.0f);
    IFE_CHECK(k::ORIENTATION_90       ==   90.0f);
    IFE_CHECK(k::ORIENTATION_180      ==  180.0f);
    IFE_CHECK(k::ORIENTATION_270      ==  270.0f);
    IFE_CHECK(k::ORIENTATION_MINUS_90 ==  -90.0f);
    IFE_CHECK(IFE::float_to_half(k::ORIENTATION_270) == 0x5C38);
}

void test_half_edges() {
    IFE_CHECK(IFE::half_to_float(0x0000) == 0.0f);
    IFE_CHECK(std::signbit(IFE::half_to_float(0x8000)));            // -0
    IFE_CHECK(IFE::half_to_float(0x0001) == std::ldexp(1.0f, -24)); // smallest subnormal
    IFE_CHECK(IFE::half_to_float(0x03FF) == std::ldexp(1023.0f, -24));
    IFE_CHECK(IFE::half_to_float(0x0400) == std::ldexp(1.0f, -14)); // smallest normal
    IFE_CHECK(IFE::half_to_float(0x7BFF) == 65504.0f);              // largest finite
    IFE_CHECK(std::isinf(IFE::half_to_float(0x7C00)));
    IFE_CHECK(std::isinf(IFE::half_to_float(0xFC00)));

    IFE_CHECK(IFE::float_to_half(65504.0f) == 0x7BFF);
    IFE_CHECK(IFE::float_to_half(1e30f)    == 0x7C00);  // overflow saturates
    IFE_CHECK(IFE::float_to_half(-1e30f)   == 0xFC00);
    IFE_CHECK(IFE::float_to_half(1e-30f)   == 0x0000);  // underflow to zero
    IFE_CHECK(std::isnan(IFE::half_to_float(IFE::float_to_half(
        std::numeric_limits<float>::quiet_NaN()))));

    // Ties round to even, as IEEE requires: 2049 sits exactly between two
    // representable halves and must land on the even one.
    IFE_CHECK(IFE::half_to_float(IFE::float_to_half(2049.0f)) == 2048.0f);
    IFE_CHECK(IFE::half_to_float(IFE::float_to_half(2051.0f)) == 2052.0f);
}

}  // namespace

int main() {
    test_whole_width_scalars();
    test_unaligned();
    test_packed_widths_touch_exact_bytes();
    test_packed_loads_do_not_over_read();
    test_wire_byte_order();
    test_tile_offset_entry_shape();
    test_half_exhaustive();
    test_orientation_constants();
    test_half_edges();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_bytes_tests: all checks passed\n");
    return 0;
}
