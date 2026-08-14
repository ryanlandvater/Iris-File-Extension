/**
 * @file ife_version_gating_tests.cpp
 * @brief The since-mechanism, executed. Compiled against a generated layer
 *        produced from the synthetic 200.0 spec in tests/fixtures/.
 *
 * The real spec is 1.0-only, so version gating has never run: every accessor
 * the production layer emits is a 1.0 accessor and no guard exists. The
 * fixture adds one 200.0 field to FILE_HEADER (RUNTIME_FLAGS) and one to the
 * LAYER_EXTENT entry (RESERVED_EXTENT) — the block-level and the stride-gated
 * cases — and this file asserts the behaviors the mechanism exists for. The
 * version is 200.0, not 1.1, deliberately: a multi-digit major proves the
 * comparison is numeric rather than lexicographic, keeping the test relevant
 * for decades.
 *
 *   1. a 1.0 file read by the 200.0 build: every 200.0 accessor is empty,
 *      every 1.0 accessor correct;
 *   2. a 200.0 file read by the same build: both;
 *   3. the same 200.0 file with the handle version forced to 1.0: only the
 *      1.0 prefix — the guard is about the version, not the file's bytes;
 *   4. an array whose stored stride is the old one reads its 1.0 fields and
 *      gates the 200.0 field out; a stride wider than the compiled entry is
 *      stepped by the stored value and the 200.0 field reads — the unknown
 *      tail is never interpreted;
 *   5. a newer-minor file's known prefix reads correctly and nothing warns
 *      or fails (append-only: the 1.0/200.0 prefix is always readable).
 *
 * The reverse direction — the production 1.0 build reading this 200.0 file —
 * is ife_version_gating_backward_tests.cpp.
 *
 * The per-version tables are pinned by static_assert: one size constant per
 * version group per structure, with the newest aliased — the boundary a
 * version-gated reader must never cross.
 */
#include "IFE_Blocks.hpp"
#include "ife_fixture_layout.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

using ::IFE::BYTE;
using ::IFE::Offset;
namespace k  = ::IFE::constants;
namespace b  = ::IFE::blocks;

constexpr std::uint32_t V1_0   = (1u << 16) | 0u;
constexpr std::uint32_t V200_0 = (200u << 16) | 0u;
constexpr std::uint32_t V200_1 = (200u << 16) | 1u;

// No byte count is pinned in this file: every size comes from the vtable
// tables — the newest version's table is the plain `header_size` /
// `entry_size` alias — and the emitted layer is cross-checked against the
// fixture builder's own derivation (ife_fixture_layout.hpp, produced by the
// same derive_layout the generator runs). The 200.0 fields begin where the
// real spec's newest version ends (the prefix boundary the builder emits);
// that relationship holds under any future spec growth, so nothing here
// names a concrete byte count.
static_assert(b::FILE_HEADER::offset::RUNTIME_FLAGS == ife_test::FILE_HEADER_PREFIX_SIZE);
static_assert(b::FILE_HEADER::header_size == b::FILE_HEADER::header_size_v200_0);
static_assert(b::LAYER_EXTENTS::LAYER_EXTENT::offset::RESERVED_EXTENT == ife_test::LAYER_EXTENT_PREFIX_SIZE);
static_assert(b::LAYER_EXTENTS::LAYER_EXTENT::entry_size == b::LAYER_EXTENTS::LAYER_EXTENT::entry_size_v200_0);
static_assert(b::FILE_HEADER::header_size == ife_test::FILE_HEADER_HEADER_SIZE);
static_assert(b::LAYER_EXTENTS::LAYER_EXTENT::entry_size == ife_test::LAYER_EXTENT_ENTRY_SIZE);
static_assert(b::FILE_HEADER::offset::RUNTIME_FLAGS == ife_test::RUNTIME_FLAGS_AT);
static_assert(b::LAYER_EXTENTS::LAYER_EXTENT::offset::RESERVED_EXTENT == ife_test::RESERVED_EXTENT_AT);

constexpr std::uint32_t RUNTIME_FLAGS_VALUE = 0xCAFEBABEu;
constexpr std::uint16_t RESERVED_EXTENT_VALUE = 0xBEEFu;

/// Block offsets for a head-to-tail file with `entry_count` extents of
/// `entry_stride` bytes and one tile-offset entry. Every number is a vtable
/// constant — nothing hardcoded.
struct Layout {
    Offset tile_table, layer_extents, tile_offsets, metadata, end;
};
constexpr Layout layout(Offset fh_size, std::uint16_t entry_stride, std::uint32_t entry_count = 2) {
    const Offset tt = fh_size;
    const Offset le = tt + b::TILE_TABLE::header_size;
    const Offset to = le + b::LAYER_EXTENTS::header_size + static_cast<Offset>(entry_count) * entry_stride;
    const Offset md = to + b::TILE_OFFSETS::header_size + b::TILE_OFFSETS::TILE_OFFSET::entry_size;
    return {tt, le, to, md, md + b::METADATA::header_size};
}

void store_header(BYTE* p, Offset fh_end, Offset tt, Offset md, std::uint32_t version) {
    ::IFE::store<std::uint32_t>(p + b::FILE_HEADER::offset::MAGIC, k::MAGIC_BYTES);
    ::IFE::store<std::uint16_t>(p + b::FILE_HEADER::offset::RECOVERY,
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_FILE_HEADER));
    ::IFE::store<std::uint64_t>(p + b::FILE_HEADER::offset::FILE_SIZE, fh_end);
    ::IFE::store<std::uint16_t>(p + b::FILE_HEADER::offset::EXTENSION_MAJOR,
                                static_cast<std::uint16_t>(version >> 16));
    ::IFE::store<std::uint16_t>(p + b::FILE_HEADER::offset::EXTENSION_MINOR,
                                static_cast<std::uint16_t>(version & 0xFFFF));
    ::IFE::store<std::uint32_t>(p + b::FILE_HEADER::offset::FILE_REVISION, 7);
    ::IFE::store<std::uint64_t>(p + b::FILE_HEADER::offset::TILE_TABLE_OFFSET, tt);
    ::IFE::store<std::uint64_t>(p + b::FILE_HEADER::offset::METADATA_OFFSET, md);
}

void store_tile_table(BYTE* p, Offset at, Offset le, Offset to) {
    ::IFE::store<std::uint64_t>(p + at + b::TILE_TABLE::offset::VALIDATION, at);
    ::IFE::store<std::uint16_t>(p + at + b::TILE_TABLE::offset::RECOVERY,
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_TILE_TABLE));
    ::IFE::store<std::uint8_t>(p + at + b::TILE_TABLE::offset::ENCODING,
                               static_cast<std::uint8_t>(k::TileEncodings::TILE_ENCODING_JPEG));
    ::IFE::store<std::uint8_t>(p + at + b::TILE_TABLE::offset::FORMAT,
                               static_cast<std::uint8_t>(k::PixelFormats::FORMAT_R8G8B8A8));
    ::IFE::store<std::uint64_t>(p + at + b::TILE_TABLE::offset::CIPHER_OFFSET, k::NULL_OFFSET);
    ::IFE::store<std::uint64_t>(p + at + b::TILE_TABLE::offset::TILE_OFFSETS_OFFSET, to);
    ::IFE::store<std::uint64_t>(p + at + b::TILE_TABLE::offset::LAYER_EXTENTS_OFFSET, le);
    ::IFE::store<std::uint32_t>(p + at + b::TILE_TABLE::offset::X_EXTENT, 4096);
    ::IFE::store<std::uint32_t>(p + at + b::TILE_TABLE::offset::Y_EXTENT, 2048);
}

void store_metadata(BYTE* p, Offset at) {
    ::IFE::store<std::uint64_t>(p + at + b::TILE_TABLE::offset::VALIDATION, at);
    ::IFE::store<std::uint16_t>(p + at + b::TILE_TABLE::offset::RECOVERY,
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_METADATA));
    ::IFE::store<std::uint16_t>(p + at + b::METADATA::offset::CODEC_MAJOR, 2);
    ::IFE::store<std::uint16_t>(p + at + b::METADATA::offset::CODEC_MINOR, 1);
    ::IFE::store<std::uint16_t>(p + at + b::METADATA::offset::CODEC_BUILD, 3);
    for (auto field : {b::METADATA::offset::ATTRIBUTES_OFFSET, b::METADATA::offset::IMAGES_OFFSET,
                       b::METADATA::offset::ICC_COLOR_OFFSET, b::METADATA::offset::ANNOTATIONS_OFFSET,
                       b::METADATA::offset::CLINICAL_OFFSET})
        ::IFE::store<std::uint64_t>(p + at + field, k::NULL_OFFSET);
    ::IFE::store<float>(p + at + b::METADATA::offset::MICRONS_PIXEL, 0.25f);
    ::IFE::store<float>(p + at + b::METADATA::offset::MAGNIFICATION, 40.0f);
}

/// A head-to-tail file: header of `fh_size` bytes, `entry_count` extents of
/// `entry_stride` bytes, one tile-offset entry. `with_200_fields` controls
/// whether the fixture's 200.0 fields are written — a 1.0 file must not
/// carry bytes past its own boundary, and the flag is how the tests build
/// both without any layout literals.
std::vector<BYTE> make_file(std::uint32_t version, Offset fh_size, std::uint16_t entry_stride,
                            bool with_200_fields, std::uint32_t entry_count = 2) {
    const auto L = layout(fh_size, entry_stride, entry_count);
    std::vector<BYTE> f(L.end, 0);
    BYTE* p = f.data();

    store_header(p, L.end, L.tile_table, L.metadata, version);
    if (with_200_fields)
        ::IFE::store<std::uint32_t>(p + b::FILE_HEADER::offset::RUNTIME_FLAGS, RUNTIME_FLAGS_VALUE);

    store_tile_table(p, L.tile_table, L.layer_extents, L.tile_offsets);

    ::IFE::store<std::uint64_t>(p + L.layer_extents + b::LAYER_EXTENTS::offset::VALIDATION, L.layer_extents);
    ::IFE::store<std::uint16_t>(p + L.layer_extents + b::LAYER_EXTENTS::offset::RECOVERY,
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_LAYER_EXTENTS));
    ::IFE::store<std::uint16_t>(p + L.layer_extents + b::LAYER_EXTENTS::offset::STRIDE, entry_stride);
    ::IFE::store<std::uint32_t>(p + L.layer_extents + b::LAYER_EXTENTS::offset::COUNT, entry_count);
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        BYTE* e = p + L.layer_extents + b::LAYER_EXTENTS::header_size + static_cast<Offset>(i) * entry_stride;
        ::IFE::store<std::uint32_t>(e + b::LAYER_EXTENTS::LAYER_EXTENT::offset::X_TILES, 8u << i);
        ::IFE::store<std::uint32_t>(e + b::LAYER_EXTENTS::LAYER_EXTENT::offset::Y_TILES, 8);
        ::IFE::store<float>(e + b::LAYER_EXTENTS::LAYER_EXTENT::offset::SCALE, static_cast<float>(i + 1));
        if (with_200_fields)
            ::IFE::store<std::uint16_t>(e + b::LAYER_EXTENTS::LAYER_EXTENT::offset::RESERVED_EXTENT,
                                        static_cast<std::uint16_t>(RESERVED_EXTENT_VALUE + i));
    }

    ::IFE::store<std::uint64_t>(p + L.tile_offsets + b::LAYER_EXTENTS::offset::VALIDATION, L.tile_offsets);
    ::IFE::store<std::uint16_t>(p + L.tile_offsets + b::LAYER_EXTENTS::offset::RECOVERY,
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_TILE_OFFSETS));
    ::IFE::store<std::uint16_t>(p + L.tile_offsets + b::LAYER_EXTENTS::offset::STRIDE, b::TILE_OFFSETS::TILE_OFFSET::entry_size);
    ::IFE::store<std::uint32_t>(p + L.tile_offsets + b::LAYER_EXTENTS::offset::COUNT, 1);
    BYTE* te = p + L.tile_offsets + b::TILE_OFFSETS::header_size;
    ::IFE::store_u40(te + b::TILE_OFFSETS::TILE_OFFSET::offset::OFFSET, 0xFEDCBA98ull);
    ::IFE::store_u24(te + b::TILE_OFFSETS::TILE_OFFSET::offset::SIZE, 0x00ABCDu);

    store_metadata(p, L.metadata);
    return f;
}

b::FILE_HEADER root(const std::vector<BYTE>& f, std::uint32_t version) {
    // FILE_HEADER is fixed at byte 0 by the spec (fixed_location).
    return b::FILE_HEADER{f.data(), 0, f.size(), version};
}

void test_1_0_file_read_by_200_0_build() {
    // The fixture's 1.0 prefix is the real spec's layout: same offsets. The
    // version gates come from the file's own version fields, so this builds a
    // file that SAYS 1.0 (the 200.0 header bytes still carry the value; the
    // handle version is what must decide).
    // The 1.0 layout is the v1_0 table entries: header_size_v1_0, entry
    // stride entry_size_v1_0. These are the only version-suffixed constants
    // in the file, and only here — constructing the 1.0 boundary has no
    // other source.
    const auto f = make_file(V1_0, b::FILE_HEADER::header_size_v1_0, b::LAYER_EXTENTS::LAYER_EXTENT::entry_size_v1_0, false);
    const auto h = root(f, V1_0);
    IFE_CHECK(h.runtime_flags() == std::nullopt);       // 200.0 field, 1.0 file
    IFE_CHECK(h.file_size() == f.size());               // 1.0 fields correct
    IFE_CHECK(h.extension_major() == 1);
    IFE_CHECK(h.extension_minor() == 0);
    const auto le = h.tile_table_offset().layer_extents_offset();
    IFE_CHECK(le.entry(0).x_tiles() == 8);
    // Empty here because the *version* gate fires -- the stride gate would
    // also refuse it, but it never gets the chance. The case that isolates
    // the stride gate is below.
    IFE_CHECK(le.entry(0).reserved_extent() == std::nullopt);
}

void test_narrow_stride_gates_a_field_the_version_allows() {
    // The one shape that isolates the stride gate: a file declaring 200.0 --
    // so the version guard passes -- whose LAYER_EXTENTS entries were written
    // at the 1.0 width. The field the version says exists is physically not
    // in the entry, and reading it would walk into the next one.
    //
    // Real files take this shape whenever a writer bumps the file version
    // without growing every array, and it is the only reason the stride half
    // of the guard exists. Without this test, deleting that half breaks
    // nothing: every other `nullopt` assertion here is already satisfied by
    // the version guard alone.
    const auto f = make_file(V200_0, b::FILE_HEADER::header_size,
                             b::LAYER_EXTENTS::LAYER_EXTENT::entry_size_v1_0, false);
    const auto h = root(f, V200_0);

    const auto le = h.tile_table_offset().layer_extents_offset();
    IFE_CHECK(le.stride() == b::LAYER_EXTENTS::LAYER_EXTENT::entry_size_v1_0);
    IFE_CHECK(le.stride() < b::LAYER_EXTENTS::LAYER_EXTENT::entry_size);   // narrower than compiled
    IFE_CHECK(le.entry(0).reserved_extent() == std::nullopt);
    IFE_CHECK(le.entry(1).reserved_extent() == std::nullopt);

    // The 1.0 fields still read, at the 1.0 stride: a narrow entry is not a
    // broken one, and validate() must not reject it.
    IFE_CHECK(le.entry(0).x_tiles() == 8);
    IFE_CHECK(le.entry(1).x_tiles() == 16);
    IFE_CHECK(static_cast<bool>(le.validate()));

    // A block-level 200.0 field is unaffected -- FILE_HEADER is not an array,
    // so only the version gates it, and this file does declare 200.0.
    IFE_CHECK(h.runtime_flags() == 0u);   // present, written as zero
}

void test_200_0_file_read_by_200_0_build() {
    const auto f = make_file(V200_0, b::FILE_HEADER::header_size, b::LAYER_EXTENTS::LAYER_EXTENT::entry_size, true);
    const auto h = root(f, V200_0);
    IFE_CHECK(h.runtime_flags() == RUNTIME_FLAGS_VALUE);    // 200.0 field reads
    IFE_CHECK(h.file_size() == f.size());                   // 1.0 fields still right
    const auto le = h.tile_table_offset().layer_extents_offset();
    IFE_CHECK(le.entry(0).reserved_extent() == RESERVED_EXTENT_VALUE);
    IFE_CHECK(le.entry(1).reserved_extent() == static_cast<std::uint16_t>(RESERVED_EXTENT_VALUE + 1));
    IFE_CHECK(le.entry(1).x_tiles() == 16);
}

void test_200_0_file_read_with_version_forced_to_1_0() {
    // The bytes are present, the version is not: the guard must be about the
    // handle version, never a peek at the field. A gated reader never reads
    // past header_size_v1_0.
    const auto f = make_file(V200_0, b::FILE_HEADER::header_size, b::LAYER_EXTENTS::LAYER_EXTENT::entry_size, true);
    const auto h = root(f, V1_0);
    IFE_CHECK(h.runtime_flags() == std::nullopt);
    IFE_CHECK(h.tile_table_offset().layer_extents_offset().entry(0).reserved_extent() == std::nullopt);
}

void test_wide_stride_is_stepped_and_tail_skipped() {
    // A stride wider than the compiled entry: the known entry plus an
    // unknown tail. Iteration must step by the STORED stride, and the known
    // fields — including the 200.0 one, which the stride covers — must read.
    const std::uint16_t wide = static_cast<std::uint16_t>(b::LAYER_EXTENTS::LAYER_EXTENT::entry_size + 4);
    const auto f = make_file(V200_0, b::FILE_HEADER::header_size, wide, true);
    const auto le = root(f, V200_0).tile_table_offset().layer_extents_offset();
    IFE_CHECK(le.stride() == wide);
    IFE_CHECK(le.entry(1).x_tiles() == 16);                          // entry 1 at +wide
    IFE_CHECK(le.entry(1).reserved_extent() == static_cast<std::uint16_t>(RESERVED_EXTENT_VALUE + 1));
}

void test_newer_minor_reads_without_error() {
    // A 200.1 file: append-only says its 200.0 prefix is readable, so this
    // build reads the known fields and deliberately raises no warning.
    const auto f = make_file(V200_1, b::FILE_HEADER::header_size, b::LAYER_EXTENTS::LAYER_EXTENT::entry_size, true);
    const auto h = root(f, V200_1);
    IFE_CHECK(static_cast<bool>(h.validate()));
    IFE_CHECK(static_cast<bool>(h.validate_deep()));
    IFE_CHECK(h.runtime_flags() == RUNTIME_FLAGS_VALUE);
    IFE_CHECK(h.tile_table_offset().layer_extents_offset().entry(0).reserved_extent() == RESERVED_EXTENT_VALUE);
}

}  // namespace

int main() {
    test_1_0_file_read_by_200_0_build();
    test_narrow_stride_gates_a_field_the_version_allows();
    test_200_0_file_read_by_200_0_build();
    test_200_0_file_read_with_version_forced_to_1_0();
    test_wide_stride_is_stepped_and_tail_skipped();
    test_newer_minor_reads_without_error();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_version_gating_tests: all checks passed\n");
    return 0;
}
