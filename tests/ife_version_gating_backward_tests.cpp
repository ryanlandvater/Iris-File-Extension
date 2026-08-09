/**
 * @file ife_version_gating_backward_tests.cpp
 * @brief The bilateral half the forward test cannot cover: the PRODUCTION
 *        1.0 build reading a file written by the synthetic 200.0 spec.
 *
 * ife_version_gating_tests.cpp proves a 200.0 build reads both old and new
 * files. This is the direction that matters in the field: files written by
 * a future version must remain readable by today's 1.0 build, because
 * append-only guarantees the 1.0 prefix never moves. This translation unit
 * compiles against the production generated layer — the one real consumers
 * links — and assembles the same 200.0 file the forward test reads. The
 * 200.0 sizes and the injected-field offsets come from
 * ife_fixture_layout.hpp — derived from the fixture spec by the same layout
 * model the generator runs — so nothing here names a byte count either.
 * The production vtables only know the 1.0 layout, which is the point under
 * test.
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
namespace vt = ::IFE::vtables;
namespace b  = ::IFE::blocks;

// The 200.0 file's layout. The sizes this build knows come from the
// production vtable tables (unchanged in 200.0 — append-only); the parts
// this build does not know come from the derived fixture header.
constexpr Offset FH_AT = 0;   // FILE_HEADER is fixed at byte 0
constexpr Offset TT_AT = ife_test::FILE_HEADER_HEADER_SIZE;                    // 38 (1.0) + RUNTIME_FLAGS
constexpr Offset LE_AT = TT_AT + vt::TILE_TABLE::header_size;
constexpr Offset TO_AT = LE_AT + vt::LAYER_EXTENTS::header_size
                         + 2 * ife_test::LAYER_EXTENT_ENTRY_SIZE;
constexpr Offset MD_AT = TO_AT + vt::TILE_OFFSETS::header_size + vt::TILE_OFFSETS::entry_size;
constexpr Offset FILE_END = MD_AT + vt::METADATA::header_size;

constexpr std::uint16_t ENTRY_STRIDE_200 = static_cast<std::uint16_t>(ife_test::LAYER_EXTENT_ENTRY_SIZE);
constexpr std::uint32_t RUNTIME_FLAGS_VALUE = 0xCAFEBABEu;  // written, never read here

std::vector<BYTE> make_200_0_file() {
    std::vector<BYTE> f(FILE_END, 0);
    BYTE* p = f.data();
    auto at = [p](Offset block, std::size_t field) { return p + block + field; };

    // ---- FILE_HEADER: the 1.0 prefix, then the field this build lacks ---- //
    ::IFE::store<std::uint32_t>(at(FH_AT, vt::FILE_HEADER::offset::MAGIC), k::MAGIC_BYTES);
    ::IFE::store<std::uint16_t>(at(FH_AT, vt::FILE_HEADER::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_FILE_HEADER));
    ::IFE::store<std::uint64_t>(at(FH_AT, vt::FILE_HEADER::offset::FILE_SIZE), FILE_END);
    ::IFE::store<std::uint16_t>(at(FH_AT, vt::FILE_HEADER::offset::EXTENSION_MAJOR), 200);
    ::IFE::store<std::uint16_t>(at(FH_AT, vt::FILE_HEADER::offset::EXTENSION_MINOR), 0);
    ::IFE::store<std::uint32_t>(at(FH_AT, vt::FILE_HEADER::offset::FILE_REVISION), 7);
    ::IFE::store<std::uint64_t>(at(FH_AT, vt::FILE_HEADER::offset::TILE_TABLE_OFFSET), TT_AT);
    ::IFE::store<std::uint64_t>(at(FH_AT, vt::FILE_HEADER::offset::METADATA_OFFSET), MD_AT);
    ::IFE::store<std::uint32_t>(p + FH_AT + ife_test::RUNTIME_FLAGS_AT, RUNTIME_FLAGS_VALUE);  // 200.0-only

    // ---- TILE_TABLE ------------------------------------------------------- //
    ::IFE::store<std::uint64_t>(at(TT_AT, vt::BLOCK::offset::VALIDATION), TT_AT);
    ::IFE::store<std::uint16_t>(at(TT_AT, vt::BLOCK::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_TILE_TABLE));
    ::IFE::store<std::uint8_t>(at(TT_AT, vt::TILE_TABLE::offset::ENCODING),
                               static_cast<std::uint8_t>(k::TileEncodings::TILE_ENCODING_JPEG));
    ::IFE::store<std::uint8_t>(at(TT_AT, vt::TILE_TABLE::offset::FORMAT),
                               static_cast<std::uint8_t>(k::PixelFormats::FORMAT_R8G8B8A8));
    ::IFE::store<std::uint64_t>(at(TT_AT, vt::TILE_TABLE::offset::CIPHER_OFFSET), k::NULL_OFFSET);
    ::IFE::store<std::uint64_t>(at(TT_AT, vt::TILE_TABLE::offset::TILE_OFFSETS_OFFSET), TO_AT);
    ::IFE::store<std::uint64_t>(at(TT_AT, vt::TILE_TABLE::offset::LAYER_EXTENTS_OFFSET), LE_AT);
    ::IFE::store<std::uint32_t>(at(TT_AT, vt::TILE_TABLE::offset::X_EXTENT), 4096);
    ::IFE::store<std::uint32_t>(at(TT_AT, vt::TILE_TABLE::offset::Y_EXTENT), 2048);

    // ---- LAYER_EXTENTS: stored stride 14, the 1.0 fields within ----------- //
    ::IFE::store<std::uint64_t>(at(LE_AT, vt::ARRAY::offset::VALIDATION), LE_AT);
    ::IFE::store<std::uint16_t>(at(LE_AT, vt::ARRAY::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_LAYER_EXTENTS));
    ::IFE::store<std::uint16_t>(at(LE_AT, vt::ARRAY::offset::STRIDE), ENTRY_STRIDE_200);
    ::IFE::store<std::uint32_t>(at(LE_AT, vt::ARRAY::offset::COUNT), 2);
    for (std::uint32_t i = 0; i < 2; ++i) {
        BYTE* e = p + LE_AT + vt::LAYER_EXTENTS::header_size + i * ENTRY_STRIDE_200;
        ::IFE::store<std::uint32_t>(e + vt::LAYER_EXTENTS::entry::offset::X_TILES, i == 0 ? 8u : 16u);
        ::IFE::store<std::uint32_t>(e + vt::LAYER_EXTENTS::entry::offset::Y_TILES, i == 0 ? 8u : 8u);
        ::IFE::store<float>(e + vt::LAYER_EXTENTS::entry::offset::SCALE, i == 0 ? 1.0f : 2.0f);
        ::IFE::store<std::uint16_t>(e + ife_test::RESERVED_EXTENT_AT, static_cast<std::uint16_t>(0xBEEFu + i));  // 200.0-only
    }

    // ---- TILE_OFFSETS: one entry ------------------------------------------ //
    ::IFE::store<std::uint64_t>(at(TO_AT, vt::ARRAY::offset::VALIDATION), TO_AT);
    ::IFE::store<std::uint16_t>(at(TO_AT, vt::ARRAY::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_TILE_OFFSETS));
    ::IFE::store<std::uint16_t>(at(TO_AT, vt::ARRAY::offset::STRIDE), vt::TILE_OFFSETS::entry_size);
    ::IFE::store<std::uint32_t>(at(TO_AT, vt::ARRAY::offset::COUNT), 1);
    BYTE* te = p + TO_AT + vt::TILE_OFFSETS::header_size;
    ::IFE::store_u40(te + vt::TILE_OFFSETS::entry::offset::OFFSET, 0xFEDCBA98ull);
    ::IFE::store_u24(te + vt::TILE_OFFSETS::entry::offset::SIZE, 0x00ABCDu);

    // ---- METADATA: no optional blocks ------------------------------------- //
    ::IFE::store<std::uint64_t>(at(MD_AT, vt::BLOCK::offset::VALIDATION), MD_AT);
    ::IFE::store<std::uint16_t>(at(MD_AT, vt::BLOCK::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_METADATA));
    ::IFE::store<std::uint16_t>(at(MD_AT, vt::METADATA::offset::CODEC_MAJOR), 2);
    ::IFE::store<std::uint16_t>(at(MD_AT, vt::METADATA::offset::CODEC_MINOR), 1);
    ::IFE::store<std::uint16_t>(at(MD_AT, vt::METADATA::offset::CODEC_BUILD), 3);
    for (auto field : {vt::METADATA::offset::ATTRIBUTES_OFFSET, vt::METADATA::offset::IMAGES_OFFSET,
                       vt::METADATA::offset::ICC_COLOR_OFFSET, vt::METADATA::offset::ANNOTATIONS_OFFSET})
        ::IFE::store<std::uint64_t>(at(MD_AT, field), k::NULL_OFFSET);
    ::IFE::store<float>(at(MD_AT, vt::METADATA::offset::MICRONS_PIXEL), 0.25f);
    ::IFE::store<float>(at(MD_AT, vt::METADATA::offset::MAGNIFICATION), 40.0f);
    return f;
}

void test_200_0_file_read_by_1_0_build() {
    const auto f = make_200_0_file();
    const auto h = b::FILE_HEADER{f.data(), FH_AT, f.size(), 0x00010000u};  // the 1.0 build

    // The newer version is visible, never rejected: the 1.0 prefix of a
    // 200.0 file must be readable because append-only froze it.
    IFE_CHECK(static_cast<bool>(h));
    IFE_CHECK(h.extension_major() == 200);
    IFE_CHECK(h.extension_minor() == 0);
    IFE_CHECK(h.file_size() == FILE_END);
    IFE_CHECK(h.file_revision() == 7);

    // Every 1.0 field reads correctly through the 200.0 layout.
    const auto tt = h.tile_table_offset();
    IFE_CHECK(static_cast<bool>(tt));
    IFE_CHECK(tt.encoding() == k::TileEncodings::TILE_ENCODING_JPEG);
    IFE_CHECK(tt.x_extent() == 4096);

    // The stored stride (14) is wider than this build's entry_size (12):
    // iteration steps by the STORED value and the 1.0 fields inside read.
    const auto le = tt.layer_extents_offset();
    // The stored stride is the 200.0 entry width, wider than the 1.0 entry
    // this build compiles against -- which is exactly what it must step by.
    IFE_CHECK(le.stride() == ENTRY_STRIDE_200);
    IFE_CHECK(le.stride() > vt::LAYER_EXTENTS::entry_size);
    IFE_CHECK(le.count() == 2);
    IFE_CHECK(le.entry(0).x_tiles() == 8);
    IFE_CHECK(le.entry(1).x_tiles() == 16);
    IFE_CHECK(le.entry(1).scale() == 2.0f);

    const auto to = tt.tile_offsets_offset();
    IFE_CHECK(to.entry(0).offset() == 0xFEDCBA98ull);
    IFE_CHECK(to.entry(0).size_field() == 0x00ABCDu);

    const auto md = h.metadata_offset();
    IFE_CHECK(md.microns_pixel() == 0.25f);
    IFE_CHECK(!static_cast<bool>(md.annotations_offset()));

    // And the whole file validates under the old build — no VERSION_TOO_NEW,
    // no BAD_STRIDE, nothing.
    IFE_CHECK(static_cast<bool>(h.validate()));
    IFE_CHECK(static_cast<bool>(h.validate_deep()));
}

}  // namespace

int main() {
    test_200_0_file_read_by_1_0_build();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_version_gating_backward_tests: all checks passed\n");
    return 0;
}
