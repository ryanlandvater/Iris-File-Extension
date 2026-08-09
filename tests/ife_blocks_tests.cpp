/**
 * @file ife_blocks_tests.cpp
 * @brief The generated block handles, executed against a real byte buffer.
 *
 * Compiling proves the emitter produced valid C++. It proves nothing about
 * whether a handle reads the right value or a validator rejects the right
 * corruption, so this builds a minimal but complete IFE file in memory, reads
 * every field back, and then breaks it one byte at a time.
 *
 * The file is assembled through the generated vtable offsets, so the test and
 * the code under test agree about layout by construction — what is being
 * checked here is the *reading and validation logic*, not the offsets. The
 * offsets are pinned separately, against the shipped implementation, by
 * ife_wire_parity_tests.cpp.
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IFE_Blocks.hpp"

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

// Byte offsets of each block in the synthetic file, laid out head to tail.
constexpr Offset FILE_HEADER_AT   = 0;
constexpr Offset TILE_TABLE_AT    = 38;    // FILE_HEADER is 38 B at byte 0
constexpr Offset LAYER_EXTENTS_AT = 82;    // TILE_TABLE is 44 B
constexpr Offset TILE_OFFSETS_AT  = 122;   // 16 B header + 2 entries x 12 B
constexpr Offset METADATA_AT      = 146;   // 16 B header + 1 entry x 8 B
constexpr Offset FILE_END         = 202;   // METADATA is 56 B

constexpr std::uint32_t VERSION_1_0 = (1u << 16) | 0u;

/// A minimal file that satisfies every structural rule: header, tile table,
/// both required arrays, and metadata with all optional offsets absent.
std::vector<BYTE> make_file() {
    std::vector<BYTE> f(FILE_END, 0);
    BYTE* p = f.data();

    auto at = [p](Offset block, std::size_t field) { return p + block + field; };

    // ---- FILE_HEADER ---------------------------------------------------- //
    ::IFE::store<std::uint32_t>(at(FILE_HEADER_AT, vt::FILE_HEADER::offset::MAGIC), k::MAGIC_BYTES);
    ::IFE::store<std::uint16_t>(at(FILE_HEADER_AT, vt::FILE_HEADER::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_FILE_HEADER));
    ::IFE::store<std::uint64_t>(at(FILE_HEADER_AT, vt::FILE_HEADER::offset::FILE_SIZE), FILE_END);
    ::IFE::store<std::uint16_t>(at(FILE_HEADER_AT, vt::FILE_HEADER::offset::EXTENSION_MAJOR), 1);
    ::IFE::store<std::uint16_t>(at(FILE_HEADER_AT, vt::FILE_HEADER::offset::EXTENSION_MINOR), 0);
    ::IFE::store<std::uint32_t>(at(FILE_HEADER_AT, vt::FILE_HEADER::offset::FILE_REVISION), 7);
    ::IFE::store<std::uint64_t>(at(FILE_HEADER_AT, vt::FILE_HEADER::offset::TILE_TABLE_OFFSET), TILE_TABLE_AT);
    ::IFE::store<std::uint64_t>(at(FILE_HEADER_AT, vt::FILE_HEADER::offset::METADATA_OFFSET), METADATA_AT);

    // ---- TILE_TABLE ----------------------------------------------------- //
    ::IFE::store<std::uint64_t>(at(TILE_TABLE_AT, vt::BLOCK::offset::VALIDATION), TILE_TABLE_AT);
    ::IFE::store<std::uint16_t>(at(TILE_TABLE_AT, vt::BLOCK::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_TILE_TABLE));
    ::IFE::store<std::uint8_t>(at(TILE_TABLE_AT, vt::TILE_TABLE::offset::ENCODING),
                               static_cast<std::uint8_t>(k::TileEncodings::TILE_ENCODING_JPEG));
    ::IFE::store<std::uint8_t>(at(TILE_TABLE_AT, vt::TILE_TABLE::offset::FORMAT),
                               static_cast<std::uint8_t>(k::PixelFormats::FORMAT_R8G8B8A8));
    ::IFE::store<std::uint64_t>(at(TILE_TABLE_AT, vt::TILE_TABLE::offset::CIPHER_OFFSET), k::NULL_OFFSET);
    ::IFE::store<std::uint64_t>(at(TILE_TABLE_AT, vt::TILE_TABLE::offset::TILE_OFFSETS_OFFSET), TILE_OFFSETS_AT);
    ::IFE::store<std::uint64_t>(at(TILE_TABLE_AT, vt::TILE_TABLE::offset::LAYER_EXTENTS_OFFSET), LAYER_EXTENTS_AT);
    ::IFE::store<std::uint32_t>(at(TILE_TABLE_AT, vt::TILE_TABLE::offset::X_EXTENT), 4096);
    ::IFE::store<std::uint32_t>(at(TILE_TABLE_AT, vt::TILE_TABLE::offset::Y_EXTENT), 2048);

    // ---- LAYER_EXTENTS: two entries ------------------------------------- //
    ::IFE::store<std::uint64_t>(at(LAYER_EXTENTS_AT, vt::ARRAY::offset::VALIDATION), LAYER_EXTENTS_AT);
    ::IFE::store<std::uint16_t>(at(LAYER_EXTENTS_AT, vt::ARRAY::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_LAYER_EXTENTS));
    ::IFE::store<std::uint16_t>(at(LAYER_EXTENTS_AT, vt::ARRAY::offset::STRIDE),
                                vt::LAYER_EXTENTS::entry_size);
    ::IFE::store<std::uint32_t>(at(LAYER_EXTENTS_AT, vt::ARRAY::offset::COUNT), 2);
    for (std::uint32_t i = 0; i < 2; ++i) {
        BYTE* e = p + LAYER_EXTENTS_AT + vt::LAYER_EXTENTS::header_size + i * vt::LAYER_EXTENTS::entry_size;
        ::IFE::store<std::uint32_t>(e + vt::LAYER_EXTENTS::entry::offset::X_TILES, 8u << i);
        ::IFE::store<std::uint32_t>(e + vt::LAYER_EXTENTS::entry::offset::Y_TILES, 4u << i);
        ::IFE::store<float>(e + vt::LAYER_EXTENTS::entry::offset::SCALE, 1.0f * static_cast<float>(i + 1));
    }

    // ---- TILE_OFFSETS: one entry, exercising the packed widths ---------- //
    ::IFE::store<std::uint64_t>(at(TILE_OFFSETS_AT, vt::ARRAY::offset::VALIDATION), TILE_OFFSETS_AT);
    ::IFE::store<std::uint16_t>(at(TILE_OFFSETS_AT, vt::ARRAY::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_TILE_OFFSETS));
    ::IFE::store<std::uint16_t>(at(TILE_OFFSETS_AT, vt::ARRAY::offset::STRIDE),
                                vt::TILE_OFFSETS::entry_size);
    ::IFE::store<std::uint32_t>(at(TILE_OFFSETS_AT, vt::ARRAY::offset::COUNT), 1);
    {
        BYTE* e = p + TILE_OFFSETS_AT + vt::TILE_OFFSETS::header_size;
        ::IFE::store_u40(e + vt::TILE_OFFSETS::entry::offset::OFFSET, 0xFEDCBA98ull);
        ::IFE::store_u24(e + vt::TILE_OFFSETS::entry::offset::SIZE, 0x00ABCDu);
    }

    // ---- METADATA: every optional offset absent ------------------------- //
    ::IFE::store<std::uint64_t>(at(METADATA_AT, vt::BLOCK::offset::VALIDATION), METADATA_AT);
    ::IFE::store<std::uint16_t>(at(METADATA_AT, vt::BLOCK::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_METADATA));
    ::IFE::store<std::uint16_t>(at(METADATA_AT, vt::METADATA::offset::CODEC_MAJOR), 2);
    ::IFE::store<std::uint16_t>(at(METADATA_AT, vt::METADATA::offset::CODEC_MINOR), 1);
    ::IFE::store<std::uint16_t>(at(METADATA_AT, vt::METADATA::offset::CODEC_BUILD), 3);
    for (auto field : {vt::METADATA::offset::ATTRIBUTES_OFFSET, vt::METADATA::offset::IMAGES_OFFSET,
                       vt::METADATA::offset::ICC_COLOR_OFFSET, vt::METADATA::offset::ANNOTATIONS_OFFSET})
        ::IFE::store<std::uint64_t>(at(METADATA_AT, field), k::NULL_OFFSET);
    ::IFE::store<float>(at(METADATA_AT, vt::METADATA::offset::MICRONS_PIXEL), 0.25f);
    ::IFE::store<float>(at(METADATA_AT, vt::METADATA::offset::MAGNIFICATION), 40.0f);

    return f;
}

b::FILE_HEADER root(const std::vector<BYTE>& f) {
    return b::FILE_HEADER{f.data(), FILE_HEADER_AT, f.size(), VERSION_1_0};
}

// Every value written above must come back out. This is what compiling
// cannot tell you: that a u40 was not read as a u64, or an enum cast from
// the wrong width.
void test_reads_what_was_written() {
    const auto f = make_file();
    const auto h = root(f);
    IFE_CHECK(static_cast<bool>(h));
    IFE_CHECK(h.file_size() == FILE_END);
    IFE_CHECK(h.file_revision() == 7);
    IFE_CHECK(h.extension_major() == 1);
    IFE_CHECK(h.extension_minor() == 0);
    IFE_CHECK(h.recovery_field() == k::RecoveryCodes::RECOVER_FILE_HEADER);

    const auto tt = h.tile_table_offset();
    IFE_CHECK(static_cast<bool>(tt));
    IFE_CHECK(tt.__offset == TILE_TABLE_AT);
    IFE_CHECK(tt.encoding() == k::TileEncodings::TILE_ENCODING_JPEG);
    IFE_CHECK(tt.format() == k::PixelFormats::FORMAT_R8G8B8A8);
    IFE_CHECK(tt.x_extent() == 4096);
    IFE_CHECK(tt.y_extent() == 2048);

    // A nullable offset that is absent yields a falsy handle, not garbage.
    IFE_CHECK(!static_cast<bool>(tt.cipher_offset()));
    IFE_CHECK(tt.cipher_offset().__offset == k::NULL_OFFSET);

    const auto le = tt.layer_extents_offset();
    IFE_CHECK(le.count() == 2);
    IFE_CHECK(le.stride() == vt::LAYER_EXTENTS::entry_size);
    IFE_CHECK(le.entry(0).x_tiles() == 8);
    IFE_CHECK(le.entry(1).x_tiles() == 16);
    IFE_CHECK(le.entry(1).y_tiles() == 8);
    IFE_CHECK(le.entry(1).scale() == 2.0f);

    // The packed widths, read through the generated accessors.
    const auto to = tt.tile_offsets_offset();
    IFE_CHECK(to.count() == 1);
    IFE_CHECK(to.entry(0).offset() == 0xFEDCBA98ull);
    IFE_CHECK(to.entry(0).size_field() == 0x00ABCDu);

    const auto md = h.metadata_offset();
    IFE_CHECK(md.codec_major() == 2 && md.codec_build() == 3);
    IFE_CHECK(md.microns_pixel() == 0.25f);
    IFE_CHECK(!static_cast<bool>(md.annotations_offset()));
}

void test_valid_file_passes() {
    const auto f = make_file();
    const auto h = root(f);
    IFE_CHECK(static_cast<bool>(h.validate()));
    const auto deep = h.validate_deep();
    IFE_CHECK(static_cast<bool>(deep));
    if (!deep)
        std::fprintf(stderr, "  deep failed: code=%d block=%s field=%s\n",
                     static_cast<int>(deep.code), deep.block, deep.field);
}

// Break one thing at a time and require the specific code. A validator that
// merely fails is not much better than one that does not run.
void test_corruption_is_caught() {
    struct Case {
        const char*   what;
        b::Check      expect;
        void        (*damage)(std::vector<BYTE>&);
    };
    const Case cases[] = {
        {"magic clobbered", b::Check::BAD_CONSTANT, [](std::vector<BYTE>& f) {
            ::IFE::store<std::uint32_t>(f.data() + vt::FILE_HEADER::offset::MAGIC, 0xDEADBEEF);
        }},
        {"root recovery tag wrong", b::Check::BAD_RECOVERY, [](std::vector<BYTE>& f) {
            ::IFE::store<std::uint16_t>(f.data() + vt::FILE_HEADER::offset::RECOVERY, 0x5599);
        }},
        {"tile table self-offset wrong", b::Check::BAD_VALIDATION, [](std::vector<BYTE>& f) {
            ::IFE::store<std::uint64_t>(f.data() + TILE_TABLE_AT + vt::BLOCK::offset::VALIDATION, 99);
        }},
        {"array stride zero", b::Check::BAD_STRIDE, [](std::vector<BYTE>& f) {
            ::IFE::store<std::uint16_t>(f.data() + LAYER_EXTENTS_AT + vt::ARRAY::offset::STRIDE, 0);
        }},
        {"array count past EOF", b::Check::ARRAY_OVERRUN, [](std::vector<BYTE>& f) {
            ::IFE::store<std::uint32_t>(f.data() + LAYER_EXTENTS_AT + vt::ARRAY::offset::COUNT, 100000);
        }},
        {"required offset points past EOF", b::Check::OUT_OF_BOUNDS, [](std::vector<BYTE>& f) {
            ::IFE::store<std::uint64_t>(f.data() + vt::FILE_HEADER::offset::TILE_TABLE_OFFSET, 1u << 20);
        }},
        // Pointing an offset at a block of the wrong type is caught by the
        // recovery tag before anything else can go wrong. Note what this
        // means for cycles: a chain can only revisit an offset if the tags
        // along it all match, so in this schema — where each block type
        // appears at most once on any path — the tag check makes a cycle
        // unreachable. CYCLE is defence in depth for a file whose tags
        // happen to line up, and is exercised directly below rather than
        // through a crafted file.
        {"offset points at a block of the wrong type", b::Check::BAD_RECOVERY,
         [](std::vector<BYTE>& f) {
            ::IFE::store<std::uint64_t>(
                f.data() + TILE_TABLE_AT + vt::TILE_TABLE::offset::LAYER_EXTENTS_OFFSET, TILE_TABLE_AT);
        }},
    };
    for (const auto& c : cases) {
        auto f = make_file();
        c.damage(f);
        const auto status = root(f).validate_deep();
        if (static_cast<bool>(status) || status.code != c.expect) {
            std::fprintf(stderr, "FAIL: %s -> code %d (wanted %d)\n",
                         c.what, static_cast<int>(status.code), static_cast<int>(c.expect));
            ++g_failures;
        }
    }
}

// A truncated file must fail at construction, not part-way through a read.
void test_truncation() {
    auto f = make_file();
    f.resize(20);                       // less than one FILE_HEADER
    const b::FILE_HEADER h{f.data(), 0, f.size(), VERSION_1_0};
    IFE_CHECK(!static_cast<bool>(h));
    IFE_CHECK(h.validate().code == b::Check::NOT_CONSTRUCTED);
}

// A stride wider than this build knows is a later version, and must be
// accepted and stepped over — the forward-compatibility guarantee.
void test_wider_stride_is_read_not_rejected() {
    constexpr std::uint16_t WIDE = 16;   // 1.0 entry is 12
    std::vector<BYTE> f(LAYER_EXTENTS_AT + vt::LAYER_EXTENTS::header_size + 2 * WIDE, 0);
    BYTE* p = f.data() + LAYER_EXTENTS_AT;
    ::IFE::store<std::uint64_t>(p + vt::ARRAY::offset::VALIDATION, LAYER_EXTENTS_AT);
    ::IFE::store<std::uint16_t>(p + vt::ARRAY::offset::RECOVERY,
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_LAYER_EXTENTS));
    ::IFE::store<std::uint16_t>(p + vt::ARRAY::offset::STRIDE, WIDE);
    ::IFE::store<std::uint32_t>(p + vt::ARRAY::offset::COUNT, 2);
    for (std::uint32_t i = 0; i < 2; ++i) {
        BYTE* e = p + vt::LAYER_EXTENTS::header_size + i * WIDE;
        ::IFE::store<std::uint32_t>(e + vt::LAYER_EXTENTS::entry::offset::X_TILES, 100 + i);
    }
    const b::LAYER_EXTENTS le{f.data(), LAYER_EXTENTS_AT, f.size(), VERSION_1_0};
    IFE_CHECK(static_cast<bool>(le.validate()));
    IFE_CHECK(le.stride() == WIDE);
    // Stepped by the stored stride, not by the compiled entry size.
    IFE_CHECK(le.entry(0).x_tiles() == 100);
    IFE_CHECK(le.entry(1).x_tiles() == 101);
}

// The cycle guard itself, since no file this schema permits can reach it.
void test_visit_path() {
    b::VisitPath path;
    IFE_CHECK(!path.contains(42));
    IFE_CHECK(path.push(42));
    IFE_CHECK(path.contains(42));
    IFE_CHECK(!path.contains(43));
    path.pop();
    IFE_CHECK(!path.contains(42));

    // A sibling revisiting the same target is not a cycle: the path pops
    // between branches. Two IMAGES entries may legitimately point at one
    // IMAGE_BYTES, and a global visited-set would have rejected that.
    IFE_CHECK(path.push(10));
    IFE_CHECK(path.push(20));
    path.pop();
    IFE_CHECK(path.push(20));
    IFE_CHECK(path.depth == 2);

    // Depth is bounded, and exhausting it reports rather than overruns.
    b::VisitPath deep;
    for (std::size_t i = 0; i < b::MAX_BLOCK_DEPTH; ++i)
        IFE_CHECK(deep.push(static_cast<Offset>(i)));
    IFE_CHECK(!deep.push(999));
}

// ---- fixtures that pin the three hand-written-layer bug classes ---------- //
//
// The extended file exercises, against the generated layer, the defects that
// shipped in src/IrisCodecExtension.cpp and were corrected alongside these
// tests:
//   1. a blob length stored through a narrower width than its field (the ICC
//      store wrote the u32 byte count through STORE_U16, so profiles >= 64 KiB
//      came back truncated);
//   2. a block whose payload was sized as title * bytes instead of
//      title + bytes (IMAGE_BYTES::size());
//   3. array entries read from the block header instead of from each entry
//      (the ANNOTATION_GROUP_SIZES readers).
constexpr std::uint16_t IMG_TITLE          = 300;    // 300 * 500 != 300 + 500
constexpr std::uint32_t IMG_DATA           = 500;
constexpr std::uint32_t ICC_BYTE_COUNT     = 70000;  // > 0xFFFF: truncation would read 4464
constexpr std::uint32_t ANNOTATION_BYTES_COUNT = 12;
constexpr std::uint32_t GROUP_PAYLOAD      = 15;     // (1 + 2*3) + (5 + 1*3)

// Head-to-tail offsets of the extended file's appended blocks, derived the
// same way make_extended_file lays them out so the tests can reference them.
constexpr Offset IMAGES_AT      = FILE_END;          // 202: end of make_file()
constexpr Offset IMAGE_BYTES_AT = IMAGES_AT + vt::IMAGES::header_size + vt::IMAGES::entry_size;
constexpr Offset ANNOTATIONS_AT = IMAGE_BYTES_AT + vt::IMAGE_BYTES::header_size + IMG_TITLE + IMG_DATA;
constexpr Offset ANNOTATION_BYTES_AT = ANNOTATIONS_AT + vt::ANNOTATIONS::header_size + vt::ANNOTATIONS::entry_size;
constexpr Offset GROUP_SIZES_AT  = ANNOTATION_BYTES_AT + vt::ANNOTATION_BYTES::header_size + ANNOTATION_BYTES_COUNT;
constexpr Offset GROUP_BYTES_AT  = GROUP_SIZES_AT + vt::ANNOTATION_GROUP_SIZES::header_size
                                   + 2 * vt::ANNOTATION_GROUP_SIZES::entry_size;
constexpr Offset ICC_AT          = GROUP_BYTES_AT + vt::ANNOTATION_GROUP_BYTES::header_size + GROUP_PAYLOAD;
constexpr Offset FILE_END2       = ICC_AT + vt::ICC_PROFILE::header_size + ICC_BYTE_COUNT;

std::vector<BYTE> make_extended_file() {
    auto f = make_file();  // skeleton: header, tile table, extents, offsets, metadata

    f.resize(FILE_END2, 0);
    BYTE* p = f.data();
    auto at = [p](Offset block, std::size_t field) { return p + block + field; };

    // ---- IMAGES: one entry -> IMAGE_BYTES ------------------------------- //
    ::IFE::store<std::uint64_t>(at(IMAGES_AT, vt::ARRAY::offset::VALIDATION), IMAGES_AT);
    ::IFE::store<std::uint16_t>(at(IMAGES_AT, vt::ARRAY::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_IMAGES));
    ::IFE::store<std::uint16_t>(at(IMAGES_AT, vt::ARRAY::offset::STRIDE), vt::IMAGES::entry_size);
    ::IFE::store<std::uint32_t>(at(IMAGES_AT, vt::ARRAY::offset::COUNT), 1);
    BYTE* ie = p + IMAGES_AT + vt::IMAGES::header_size;
    ::IFE::store<std::uint64_t>(ie + vt::IMAGES::entry::offset::BYTES_OFFSET, IMAGE_BYTES_AT);
    ::IFE::store<std::uint32_t>(ie + vt::IMAGES::entry::offset::WIDTH, 256);
    ::IFE::store<std::uint32_t>(ie + vt::IMAGES::entry::offset::HEIGHT, 512);
    ::IFE::store<std::uint8_t>(ie + vt::IMAGES::entry::offset::ENCODING,
                               static_cast<std::uint8_t>(k::ImageEncodings::IMAGE_ENCODING_JPEG));
    ::IFE::store<std::uint8_t>(ie + vt::IMAGES::entry::offset::FORMAT,
                               static_cast<std::uint8_t>(k::PixelFormats::FORMAT_R8G8B8A8));
    ::IFE::store<std::uint16_t>(ie + vt::IMAGES::entry::offset::ORIENTATION, 0x55A0);  // 90.0f

    // ---- IMAGE_BYTES: 300 B label + 500 B stream (sum, not product) ----- //
    ::IFE::store<std::uint64_t>(at(IMAGE_BYTES_AT, vt::BLOCK::offset::VALIDATION), IMAGE_BYTES_AT);
    ::IFE::store<std::uint16_t>(at(IMAGE_BYTES_AT, vt::BLOCK::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_IMAGE_BYTES));
    ::IFE::store<std::uint16_t>(at(IMAGE_BYTES_AT, vt::IMAGE_BYTES::offset::TITLE_SIZE), IMG_TITLE);
    ::IFE::store<std::uint32_t>(at(IMAGE_BYTES_AT, vt::IMAGE_BYTES::offset::IMAGE_SIZE), IMG_DATA);
    std::memset(p + IMAGE_BYTES_AT + vt::IMAGE_BYTES::header_size, 'L', IMG_TITLE);
    std::memset(p + IMAGE_BYTES_AT + vt::IMAGE_BYTES::header_size + IMG_TITLE, 0xAB, IMG_DATA);

    // ---- ANNOTATIONS: one entry -> ANNOTATION_BYTES, two groups --------- //
    ::IFE::store<std::uint64_t>(at(ANNOTATIONS_AT, vt::ARRAY::offset::VALIDATION), ANNOTATIONS_AT);
    ::IFE::store<std::uint16_t>(at(ANNOTATIONS_AT, vt::ARRAY::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_ANNOTATIONS));
    ::IFE::store<std::uint16_t>(at(ANNOTATIONS_AT, vt::ARRAY::offset::STRIDE), vt::ANNOTATIONS::entry_size);
    ::IFE::store<std::uint32_t>(at(ANNOTATIONS_AT, vt::ARRAY::offset::COUNT), 1);
    ::IFE::store<std::uint64_t>(at(ANNOTATIONS_AT, vt::ANNOTATIONS::offset::GROUP_SIZES_OFFSET), GROUP_SIZES_AT);
    ::IFE::store<std::uint64_t>(at(ANNOTATIONS_AT, vt::ANNOTATIONS::offset::GROUP_BYTES_OFFSET), GROUP_BYTES_AT);
    BYTE* ae = p + ANNOTATIONS_AT + vt::ANNOTATIONS::header_size;
    ::IFE::store_u24(ae + vt::ANNOTATIONS::entry::offset::IDENTIFIER, 42);
    ::IFE::store<std::uint64_t>(ae + vt::ANNOTATIONS::entry::offset::BYTES_OFFSET, ANNOTATION_BYTES_AT);
    ::IFE::store<std::uint8_t>(ae + vt::ANNOTATIONS::entry::offset::FORMAT,
                               static_cast<std::uint8_t>(k::AnnotationTypes::ANNOTATION_PNG));
    ::IFE::store<float>(ae + vt::ANNOTATIONS::entry::offset::X_LOCATION, 1.5f);
    ::IFE::store<float>(ae + vt::ANNOTATIONS::entry::offset::Y_LOCATION, 2.5f);
    ::IFE::store<float>(ae + vt::ANNOTATIONS::entry::offset::X_SIZE, 3.5f);
    ::IFE::store<float>(ae + vt::ANNOTATIONS::entry::offset::Y_SIZE, 4.5f);
    ::IFE::store<std::uint32_t>(ae + vt::ANNOTATIONS::entry::offset::PIXEL_WIDTH, 100);
    ::IFE::store<std::uint32_t>(ae + vt::ANNOTATIONS::entry::offset::PIXEL_HEIGHT, 200);
    ::IFE::store_u24(ae + vt::ANNOTATIONS::entry::offset::PARENT_ID, 0xFFFFFF);

    // ---- ANNOTATION_BYTES: 12 B stream ---------------------------------- //
    ::IFE::store<std::uint64_t>(at(ANNOTATION_BYTES_AT, vt::BYTE_ARRAY::offset::VALIDATION), ANNOTATION_BYTES_AT);
    ::IFE::store<std::uint16_t>(at(ANNOTATION_BYTES_AT, vt::BYTE_ARRAY::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_ANNOTATION_BYTES));
    ::IFE::store<std::uint32_t>(at(ANNOTATION_BYTES_AT, vt::BYTE_ARRAY::offset::COUNT), ANNOTATION_BYTES_COUNT);
    std::memset(p + ANNOTATION_BYTES_AT + vt::ANNOTATION_BYTES::header_size, 0xCD, ANNOTATION_BYTES_COUNT);

    // ---- GROUP_SIZES: two entries ('A' + 2 members, "ZEBRA" + 1 member) - //
    ::IFE::store<std::uint64_t>(at(GROUP_SIZES_AT, vt::ARRAY::offset::VALIDATION), GROUP_SIZES_AT);
    ::IFE::store<std::uint16_t>(at(GROUP_SIZES_AT, vt::ARRAY::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_ANNOTATION_GROUP_SIZES));
    ::IFE::store<std::uint16_t>(at(GROUP_SIZES_AT, vt::ARRAY::offset::STRIDE), vt::ANNOTATION_GROUP_SIZES::entry_size);
    ::IFE::store<std::uint32_t>(at(GROUP_SIZES_AT, vt::ARRAY::offset::COUNT), 2);
    BYTE* g0 = p + GROUP_SIZES_AT + vt::ANNOTATION_GROUP_SIZES::header_size;
    ::IFE::store<std::uint16_t>(g0 + vt::ANNOTATION_GROUP_SIZES::entry::offset::TITLE_SIZE, 1);
    ::IFE::store<std::uint32_t>(g0 + vt::ANNOTATION_GROUP_SIZES::entry::offset::MEMBER_COUNT, 2);
    ::IFE::store<std::uint16_t>(g0 + vt::ANNOTATION_GROUP_SIZES::entry_size + vt::ANNOTATION_GROUP_SIZES::entry::offset::TITLE_SIZE, 5);
    ::IFE::store<std::uint32_t>(g0 + vt::ANNOTATION_GROUP_SIZES::entry_size + vt::ANNOTATION_GROUP_SIZES::entry::offset::MEMBER_COUNT, 1);

    // ---- GROUP_BYTES: 'A' + 2 members, "ZEBRA" + 1 member = 15 B ------- //
    ::IFE::store<std::uint64_t>(at(GROUP_BYTES_AT, vt::BYTE_ARRAY::offset::VALIDATION), GROUP_BYTES_AT);
    ::IFE::store<std::uint16_t>(at(GROUP_BYTES_AT, vt::BYTE_ARRAY::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_ANNOTATION_GROUP_BYTES));
    ::IFE::store<std::uint32_t>(at(GROUP_BYTES_AT, vt::BYTE_ARRAY::offset::COUNT), GROUP_PAYLOAD);
    BYTE* gb = p + GROUP_BYTES_AT + vt::ANNOTATION_GROUP_BYTES::header_size;
    *gb = 'A';
    std::memset(gb + 1, 1, 6);          // two 3-byte member identifiers
    std::memcpy(gb + 7, "ZEBRA", 5);
    std::memset(gb + 12, 2, 3);         // one 3-byte member identifier

    // ---- ICC_PROFILE: 70,000 B (the u16-truncation class) -------------- //
    ::IFE::store<std::uint64_t>(at(ICC_AT, vt::BYTE_ARRAY::offset::VALIDATION), ICC_AT);
    ::IFE::store<std::uint16_t>(at(ICC_AT, vt::BYTE_ARRAY::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_ICC_PROFILE));
    ::IFE::store<std::uint32_t>(at(ICC_AT, vt::BYTE_ARRAY::offset::COUNT), ICC_BYTE_COUNT);
    std::memset(p + ICC_AT + vt::ICC_PROFILE::header_size, 0xEE, ICC_BYTE_COUNT);

    // ---- point METADATA at the new blocks, fix the file size ------------ //
    ::IFE::store<std::uint64_t>(at(METADATA_AT, vt::METADATA::offset::IMAGES_OFFSET), IMAGES_AT);
    ::IFE::store<std::uint64_t>(at(METADATA_AT, vt::METADATA::offset::ICC_COLOR_OFFSET), ICC_AT);
    ::IFE::store<std::uint64_t>(at(METADATA_AT, vt::METADATA::offset::ANNOTATIONS_OFFSET), ANNOTATIONS_AT);
    // FILE_END2, not make_file()'s FILE_END: handles bound against the buffer
    // length today, but 4.4 validates against this field, and a header
    // declaring 202 bytes over an 86 KB file would fail there instead of here.
    ::IFE::store<std::uint64_t>(at(FILE_HEADER_AT, vt::FILE_HEADER::offset::FILE_SIZE), FILE_END2);
    return f;
}

void test_large_blob_length_reads_full_u32() {
    const auto f = make_extended_file();
    const auto h = root(f);
    const auto icc = h.metadata_offset().icc_color_offset();
    IFE_CHECK(static_cast<bool>(icc));
    const auto span = icc.bytes();
    // The v1 store wrote this u32 length through STORE_U16; 70000 & 0xFFFF
    // is 4464, so a truncated reader reports 4464 and validates against it.
    IFE_CHECK(span.size == ICC_BYTE_COUNT);
    IFE_CHECK(span.data == f.data() + ICC_AT + vt::ICC_PROFILE::header_size);
}

void test_image_bytes_is_sum_not_product() {
    const auto f = make_extended_file();
    const auto h = root(f);
    const auto im = h.metadata_offset().images_offset();
    IFE_CHECK(im.count() == 1);
    const auto ib = im.entry(0).bytes_offset();
    IFE_CHECK(static_cast<bool>(ib));
    IFE_CHECK(ib.title_size() == IMG_TITLE);
    IFE_CHECK(ib.image_size() == IMG_DATA);
    // The fixture only fits because payload = title + data (816 B here); a
    // title * data reading would claim 150,016 B past the header. The block
    // ends exactly where the next block begins.
    IFE_CHECK(IMAGE_BYTES_AT + vt::IMAGE_BYTES::header_size + IMG_TITLE + IMG_DATA == ANNOTATIONS_AT);
    IFE_CHECK(im.entry(0).orientation() == 90.0f);  // 0x55A0 through load_f16
}

void test_annotation_groups_read_from_entries() {
    const auto f = make_extended_file();
    const auto h = root(f);
    const auto an = h.metadata_offset().annotations_offset();
    IFE_CHECK(an.count() == 1);
    const auto e0 = an.entry(0);
    IFE_CHECK(e0.identifier() == 42);
    IFE_CHECK(e0.format() == k::AnnotationTypes::ANNOTATION_PNG);
    IFE_CHECK(e0.parent_id() == 0xFFFFFF);
    IFE_CHECK(e0.bytes_offset().bytes().size == ANNOTATION_BYTES_COUNT);

    // The v1 readers pulled every entry's sizes from the block header; a
    // regenerated version of that bug would make these read VALIDATION bytes.
    const auto gs = an.group_sizes_offset();
    IFE_CHECK(gs.count() == 2);
    IFE_CHECK(gs.entry(0).title_size() == 1);
    IFE_CHECK(gs.entry(0).member_count() == 2);
    IFE_CHECK(gs.entry(1).title_size() == 5);
    IFE_CHECK(gs.entry(1).member_count() == 1);
    IFE_CHECK(an.group_bytes_offset().bytes().size == GROUP_PAYLOAD);

    // The whole extended file — entries, groups, blob — deep-validates.
    IFE_CHECK(static_cast<bool>(h.validate_deep()));
}

// ---- generated writers (4.2d) -------------------------------------------- //
//
// Every other fixture in this file hand-assembles bytes with ::IFE::store<>,
// which tests the readers against a buffer the test itself laid out. This one
// builds a whole file through the generated store() functions instead -- all
// sixteen blocks, every offset edge connected -- then deep-validates it and
// reads every field back. It is the first thing to execute the writers at all.
//
// Offsets come from size_of(), which is the point of size_of(): a caller
// places blocks without knowing a single layout constant. The one exception is
// IMAGE_BYTES, whose payload the schema does not describe (TITLE_SIZE bytes of
// label followed by IMAGE_SIZE bytes of stream); size_of() returns its header
// alone and placing the payload is the runtime's job, so the test does it.
void test_generated_writers_round_trip() {
    constexpr std::uint16_t KEY_LEN    = 7;    // "SCANNER"
    constexpr std::uint32_t VALUE_LEN  = 6;    // "TestCo"
    constexpr std::uint16_t TITLE_LEN  = 5;    // "label"
    constexpr std::uint32_t STREAM_LEN = 9;
    constexpr std::uint32_t ICC_LEN    = 12;
    constexpr std::uint32_t ANNO_LEN   = 7;
    constexpr std::uint32_t GROUP_LEN  = 16;   // (3 + 1*3) + (4 + 2*3)

    const b::LayerExtentEntry extents[2] = {
        {.X_TILES = 4, .Y_TILES = 3, .SCALE = 1.0f},
        {.X_TILES = 8, .Y_TILES = 6, .SCALE = 2.0f},
    };
    const b::TileOffsetEntry tiles[3] = {
        {.OFFSET = 0x11223344ull,     .SIZE = 0x00ABCDEFu},
        {.OFFSET = 0xFFFFFFFFFFull,   .SIZE = 0x00FFFFFFu},   // u40 / u24 maxima
        {.OFFSET = 1,                 .SIZE = 2},
    };
    const b::AttributeSizeEntry sizes[1] = {{.KEY_SIZE = KEY_LEN, .VALUE_SIZE = VALUE_LEN}};
    const b::AnnotationGroupSizeEntry groups[2] = {
        {.TITLE_SIZE = 3, .MEMBER_COUNT = 1},
        {.TITLE_SIZE = 4, .MEMBER_COUNT = 2},
    };

    // Payload buffers the blob writers copy from.
    BYTE attr_bytes[KEY_LEN + VALUE_LEN];
    std::memcpy(attr_bytes, "SCANNER", KEY_LEN);
    std::memcpy(attr_bytes + KEY_LEN, "TestCo", VALUE_LEN);
    BYTE icc[ICC_LEN];     std::memset(icc, 0xEE, sizeof(icc));
    BYTE anno[ANNO_LEN];   std::memset(anno, 0xCD, sizeof(anno));
    BYTE gbytes[GROUP_LEN];std::memset(gbytes, 0x5A, sizeof(gbytes));

    // ---- CreateInfos, with offsets filled in once the layout is known ---- //
    b::FileHeaderCreateInfo           header{};
    b::TileTableCreateInfo            table{};
    b::CipherCreateInfo               cipher{};
    b::LayerExtentsCreateInfo         layers{.entries = extents, .count = 2};
    b::TileOffsetsCreateInfo          offsets{.entries = tiles, .count = 3};
    b::MetadataCreateInfo             meta{};
    b::AttributesCreateInfo           attrs{};
    b::AttributeSizesCreateInfo       attr_sizes{.entries = sizes, .count = 1};
    b::AttributeBytesCreateInfo       attr_blob{.bytes = attr_bytes, .count = sizeof(attr_bytes)};
    b::ImagesCreateInfo               images{};
    b::ImageBytesCreateInfo           image_bytes{.TITLE_SIZE = TITLE_LEN, .IMAGE_SIZE = STREAM_LEN};
    b::IccProfileCreateInfo           icc_info{.bytes = icc, .count = ICC_LEN};
    b::AnnotationsCreateInfo          annotations{};
    b::AnnotationBytesCreateInfo      anno_blob{.bytes = anno, .count = ANNO_LEN};
    b::AnnotationGroupSizesCreateInfo group_sizes{.entries = groups, .count = 2};
    b::AnnotationGroupBytesCreateInfo group_blob{.bytes = gbytes, .count = GROUP_LEN};

    // ---- place every block head to tail, using size_of alone -------------- //
    Offset at = 0;
    auto place = [&at](::IFE::Size bytes) { const Offset here = at; at += bytes; return here; };

    const Offset header_at      = place(b::size_of(header));
    const Offset table_at       = place(b::size_of(table));
    const Offset cipher_at      = place(b::size_of(cipher));
    const Offset layers_at      = place(b::size_of(layers));
    const Offset offsets_at     = place(b::size_of(offsets));
    const Offset meta_at        = place(b::size_of(meta));
    const Offset attrs_at       = place(b::size_of(attrs));
    const Offset attr_sizes_at  = place(b::size_of(attr_sizes));
    const Offset attr_blob_at   = place(b::size_of(attr_blob));
    const Offset images_at      = place(b::size_of(images) + vt::IMAGES::entry_size);
    // Header plus the payload the schema does not describe.
    const Offset image_bytes_at = place(b::size_of(image_bytes) + TITLE_LEN + STREAM_LEN);
    const Offset icc_at         = place(b::size_of(icc_info));
    const Offset annotations_at = place(b::size_of(annotations) + vt::ANNOTATIONS::entry_size);
    const Offset anno_blob_at   = place(b::size_of(anno_blob));
    const Offset group_sizes_at = place(b::size_of(group_sizes));
    const Offset group_blob_at  = place(b::size_of(group_blob));
    const ::IFE::Size file_size = at;

    // Entries that carry offset edges of their own.
    const b::ImageEntry image_entries[1] = {{
        .BYTES_OFFSET = image_bytes_at, .WIDTH = 256, .HEIGHT = 512,
        .ENCODING = k::ImageEncodings::IMAGE_ENCODING_JPEG,
        .FORMAT = k::PixelFormats::FORMAT_R8G8B8A8, .ORIENTATION = 90.0f,
    }};
    const b::AnnotationEntry anno_entries[1] = {{
        .IDENTIFIER = 42, .BYTES_OFFSET = anno_blob_at,
        .FORMAT = k::AnnotationTypes::ANNOTATION_PNG,
        .X_LOCATION = 1.5f, .Y_LOCATION = 2.5f, .X_SIZE = 3.5f, .Y_SIZE = 4.5f,
        .PIXEL_WIDTH = 100, .PIXEL_HEIGHT = 200, .PARENT_ID = 0xFFFFFF,
    }};
    images.entries = image_entries;           images.count = 1;
    annotations.entries = anno_entries;       annotations.count = 1;

    header = {.FILE_SIZE = file_size, .EXTENSION_MAJOR = 1, .EXTENSION_MINOR = 0,
              .FILE_REVISION = 7, .TILE_TABLE_OFFSET = table_at, .METADATA_OFFSET = meta_at};
    table  = {.ENCODING = k::TileEncodings::TILE_ENCODING_JPEG,
              .FORMAT = k::PixelFormats::FORMAT_R8G8B8A8,
              .CIPHER_OFFSET = cipher_at, .TILE_OFFSETS_OFFSET = offsets_at,
              .LAYER_EXTENTS_OFFSET = layers_at, .X_EXTENT = 2048, .Y_EXTENT = 1536};
    meta   = {.CODEC_MAJOR = 2, .CODEC_MINOR = 1, .CODEC_BUILD = 9,
              .ATTRIBUTES_OFFSET = attrs_at, .IMAGES_OFFSET = images_at,
              .ICC_COLOR_OFFSET = icc_at, .ANNOTATIONS_OFFSET = annotations_at,
              .MICRONS_PIXEL = 0.25f, .MAGNIFICATION = 40.0f};
    attrs  = {.FORMAT = k::MetadataFormats::METADATA_FREE_TEXT, .VERSION = 1,
              .SIZES_OFFSET = attr_sizes_at, .BYTES_OFFSET = attr_blob_at};
    annotations.GROUP_SIZES_OFFSET = group_sizes_at;
    annotations.GROUP_BYTES_OFFSET = group_blob_at;

    // ---- write, checking each store reports success ---------------------- //
    std::vector<BYTE> f(file_size, 0);
    BYTE* p = f.data();
    IFE_CHECK(static_cast<bool>(b::store(p, header_at,      header)));
    IFE_CHECK(static_cast<bool>(b::store(p, table_at,       table)));
    IFE_CHECK(static_cast<bool>(b::store(p, cipher_at,      cipher)));
    IFE_CHECK(static_cast<bool>(b::store(p, layers_at,      layers)));
    IFE_CHECK(static_cast<bool>(b::store(p, offsets_at,     offsets)));
    IFE_CHECK(static_cast<bool>(b::store(p, meta_at,        meta)));
    IFE_CHECK(static_cast<bool>(b::store(p, attrs_at,       attrs)));
    IFE_CHECK(static_cast<bool>(b::store(p, attr_sizes_at,  attr_sizes)));
    IFE_CHECK(static_cast<bool>(b::store(p, attr_blob_at,   attr_blob)));
    IFE_CHECK(static_cast<bool>(b::store(p, images_at,      images)));
    IFE_CHECK(static_cast<bool>(b::store(p, image_bytes_at, image_bytes)));
    IFE_CHECK(static_cast<bool>(b::store(p, icc_at,         icc_info)));
    IFE_CHECK(static_cast<bool>(b::store(p, annotations_at, annotations)));
    IFE_CHECK(static_cast<bool>(b::store(p, anno_blob_at,   anno_blob)));
    IFE_CHECK(static_cast<bool>(b::store(p, group_sizes_at, group_sizes)));
    IFE_CHECK(static_cast<bool>(b::store(p, group_blob_at,  group_blob)));
    // IMAGE_BYTES' payload: the runtime's to place, per the note above.
    std::memset(p + image_bytes_at + vt::IMAGE_BYTES::header_size, 'L', TITLE_LEN);
    std::memset(p + image_bytes_at + vt::IMAGE_BYTES::header_size + TITLE_LEN, 0xAB, STREAM_LEN);

    // ---- the whole graph validates ---------------------------------------- //
    const b::FILE_HEADER root{p, header_at, file_size, b::VERSION_WRITTEN};
    const auto deep = root.validate_deep();
    IFE_CHECK(static_cast<bool>(deep));
    if (!deep) std::fprintf(stderr, "  deep validation failed in %s.%s\n", deep.block, deep.field);

    // ---- read every field back through the handles ------------------------ //
    IFE_CHECK(root.file_size() == file_size);
    IFE_CHECK(root.file_revision() == 7);
    IFE_CHECK(root.extension_major() == 1 && root.extension_minor() == 0);

    const auto tt = root.tile_table_offset();
    IFE_CHECK(tt.encoding() == k::TileEncodings::TILE_ENCODING_JPEG);
    IFE_CHECK(tt.format() == k::PixelFormats::FORMAT_R8G8B8A8);
    IFE_CHECK(tt.x_extent() == 2048 && tt.y_extent() == 1536);

    const auto le = tt.layer_extents_offset();
    IFE_CHECK(le.count() == 2);
    IFE_CHECK(le.stride() == vt::LAYER_EXTENTS::entry_size);
    IFE_CHECK(le.entry(0).x_tiles() == 4 && le.entry(0).scale() == 1.0f);
    IFE_CHECK(le.entry(1).y_tiles() == 6 && le.entry(1).scale() == 2.0f);

    // The packed widths, at their maxima: a u40 offset and a u24 size.
    const auto to = tt.tile_offsets_offset();
    IFE_CHECK(to.count() == 3);
    IFE_CHECK(to.entry(1).offset() == 0xFFFFFFFFFFull);
    IFE_CHECK(to.entry(1).size_field() == 0x00FFFFFFu);
    IFE_CHECK(to.entry(0).offset() == 0x11223344ull);

    const auto md = root.metadata_offset();
    IFE_CHECK(md.codec_major() == 2 && md.codec_build() == 9);
    IFE_CHECK(md.microns_pixel() == 0.25f && md.magnification() == 40.0f);

    const auto at_blk = md.attributes_offset();
    IFE_CHECK(at_blk.format() == k::MetadataFormats::METADATA_FREE_TEXT);
    IFE_CHECK(at_blk.sizes_offset().entry(0).key_size() == KEY_LEN);
    IFE_CHECK(at_blk.sizes_offset().entry(0).value_size() == VALUE_LEN);
    const auto blob = at_blk.bytes_offset().bytes();
    IFE_CHECK(blob.size == sizeof(attr_bytes));
    IFE_CHECK(std::memcmp(blob.data, attr_bytes, sizeof(attr_bytes)) == 0);

    const auto im = md.images_offset();
    IFE_CHECK(im.count() == 1);
    IFE_CHECK(im.entry(0).width() == 256 && im.entry(0).height() == 512);
    IFE_CHECK(im.entry(0).orientation() == 90.0f);       // f16 through the wire
    IFE_CHECK(im.entry(0).bytes_offset().title_size() == TITLE_LEN);
    IFE_CHECK(im.entry(0).bytes_offset().image_size() == STREAM_LEN);

    IFE_CHECK(md.icc_color_offset().bytes().size == ICC_LEN);
    IFE_CHECK(std::memcmp(md.icc_color_offset().bytes().data, icc, ICC_LEN) == 0);

    const auto an = md.annotations_offset();
    IFE_CHECK(an.count() == 1);
    IFE_CHECK(an.entry(0).identifier() == 42);           // u24
    IFE_CHECK(an.entry(0).parent_id() == 0xFFFFFF);      // u24 maximum
    IFE_CHECK(an.entry(0).format() == k::AnnotationTypes::ANNOTATION_PNG);
    IFE_CHECK(an.entry(0).x_location() == 1.5f && an.entry(0).y_size() == 4.5f);
    IFE_CHECK(an.entry(0).bytes_offset().bytes().size == ANNO_LEN);
    IFE_CHECK(an.group_sizes_offset().count() == 2);
    IFE_CHECK(an.group_sizes_offset().entry(1).title_size() == 4);
    IFE_CHECK(an.group_sizes_offset().entry(1).member_count() == 2);
    IFE_CHECK(an.group_bytes_offset().bytes().size == GROUP_LEN);
}

// store() must write exactly size_of() bytes -- not one more.
//
// The round-trip above cannot see an over-write, and this is not hypothetical:
// emitting a u24 field through store<std::uint32_t> passes every assertion in
// it. store and load agree (the spilled fourth byte is zero and load_u24 never
// looks at it), and the byte it clobbers belongs to a block written later,
// which repairs the damage before anything reads it. Ordering hides the bug.
//
// Poison either side and check the untouched bytes, exactly as the byte-level
// tests guard a field. Guards prove nothing was *written* out of bounds, which
// is precisely the property at issue here; the over-*read* case is ASan's job
// and is covered where the loads are tested.
template <typename CreateInfo>
void check_writes_exactly(const char* what, const CreateInfo& info) {
    constexpr BYTE   POISON = 0xA5;
    constexpr Offset PAD    = 64;
    const ::IFE::Size span  = b::size_of(info);

    std::vector<BYTE> buf(PAD + span + PAD, POISON);
    const auto status = b::store(buf.data(), PAD, info);
    if (!status) {
        std::fprintf(stderr, "FAIL: %s did not store (%s.%s)\n", what, status.block, status.field);
        ++g_failures;
        return;
    }
    for (std::size_t i = 0; i < PAD; ++i) {
        if (buf[i] != POISON) {
            std::fprintf(stderr, "FAIL: %s wrote %zu bytes BEFORE its offset\n", what, PAD - i);
            ++g_failures;
            break;
        }
    }
    for (std::size_t i = PAD + span; i < buf.size(); ++i) {
        if (buf[i] != POISON) {
            std::fprintf(stderr, "FAIL: %s wrote past size_of() at +%zu\n", what, i - (PAD + span));
            ++g_failures;
            break;
        }
    }
}

void test_writers_stay_within_size_of() {
    const b::LayerExtentEntry extents[2] = {{.X_TILES = 4, .Y_TILES = 3, .SCALE = 1.0f},
                                            {.X_TILES = 8, .Y_TILES = 6, .SCALE = 2.0f}};
    // Packed widths at their maxima: the values whose width a wrong store
    // would exceed. The last entry is the one that spills past the block.
    const b::TileOffsetEntry tiles[3] = {{.OFFSET = 0xFFFFFFFFFFull, .SIZE = 0x00FFFFFFu},
                                         {.OFFSET = 0xFFFFFFFFFFull, .SIZE = 0x00FFFFFFu},
                                         {.OFFSET = 0xFFFFFFFFFFull, .SIZE = 0x00FFFFFFu}};
    const b::AnnotationEntry annos[2] = {
        {.IDENTIFIER = 0xFFFFFF, .FORMAT = k::AnnotationTypes::ANNOTATION_PNG,
         .PARENT_ID = 0xFFFFFF},
        {.IDENTIFIER = 0xFFFFFF, .FORMAT = k::AnnotationTypes::ANNOTATION_PNG,
         .PARENT_ID = 0xFFFFFF},
    };
    const b::AttributeSizeEntry sizes[1]        = {{.KEY_SIZE = 7, .VALUE_SIZE = 6}};
    const b::AnnotationGroupSizeEntry groups[2] = {{.TITLE_SIZE = 3, .MEMBER_COUNT = 1},
                                                   {.TITLE_SIZE = 4, .MEMBER_COUNT = 2}};
    const b::ImageEntry image_entries[1] = {{.WIDTH = 256, .HEIGHT = 512, .ORIENTATION = 90.0f}};
    BYTE payload[13];
    std::memset(payload, 0x3C, sizeof(payload));

    check_writes_exactly("FILE_HEADER",            b::FileHeaderCreateInfo{.FILE_SIZE = 4096});
    check_writes_exactly("TILE_TABLE",             b::TileTableCreateInfo{.X_EXTENT = 1, .Y_EXTENT = 1});
    check_writes_exactly("CIPHER",                 b::CipherCreateInfo{});
    check_writes_exactly("METADATA",               b::MetadataCreateInfo{});
    check_writes_exactly("ATTRIBUTES",             b::AttributesCreateInfo{});
    check_writes_exactly("LAYER_EXTENTS",          b::LayerExtentsCreateInfo{.entries = extents, .count = 2});
    check_writes_exactly("TILE_OFFSETS",           b::TileOffsetsCreateInfo{.entries = tiles, .count = 3});
    check_writes_exactly("ATTRIBUTE_SIZES",        b::AttributeSizesCreateInfo{.entries = sizes, .count = 1});
    check_writes_exactly("ATTRIBUTE_BYTES",        b::AttributeBytesCreateInfo{.bytes = payload, .count = sizeof(payload)});
    check_writes_exactly("IMAGES",                 b::ImagesCreateInfo{.entries = image_entries, .count = 1});
    check_writes_exactly("IMAGE_BYTES",            b::ImageBytesCreateInfo{.TITLE_SIZE = 0, .IMAGE_SIZE = 0});
    check_writes_exactly("ICC_PROFILE",            b::IccProfileCreateInfo{.bytes = payload, .count = sizeof(payload)});
    check_writes_exactly("ANNOTATIONS",            b::AnnotationsCreateInfo{.entries = annos, .count = 2});
    check_writes_exactly("ANNOTATION_BYTES",       b::AnnotationBytesCreateInfo{.bytes = payload, .count = sizeof(payload)});
    check_writes_exactly("ANNOTATION_GROUP_SIZES", b::AnnotationGroupSizesCreateInfo{.entries = groups, .count = 2});
    check_writes_exactly("ANNOTATION_GROUP_BYTES", b::AnnotationGroupBytesCreateInfo{.bytes = payload, .count = sizeof(payload)});
}

// The hook decision 4.0-B requires: absent, store() costs one null check;
// attached, it is consulted and its verdict is what store() returns. Both
// paths asserted, because a hook that is never called is the failure mode a
// dispatch point has.
int g_hook_calls = 0;

b::Status reject_tile_table(const b::TileTableCreateInfo& __info, ::IFE::Offset __at,
                            const b::ValidationHooks*) noexcept {
    ++g_hook_calls;
    // A conformance rule the structural validator has no business knowing.
    if (__info.X_EXTENT == 0)
        return {b::Check::OUT_OF_BOUNDS, "TILE_TABLE", "X_EXTENT", 0, 1, __at};
    return {};
}

void test_validation_hook_is_dispatched() {
    std::vector<BYTE> f(256, 0);
    b::TileTableCreateInfo info{.ENCODING = k::TileEncodings::TILE_ENCODING_JPEG,
                                .FORMAT = k::PixelFormats::FORMAT_R8G8B8A8,
                                .TILE_OFFSETS_OFFSET = 128, .LAYER_EXTENTS_OFFSET = 160,
                                .X_EXTENT = 0, .Y_EXTENT = 16};

    // Detached: a violating CreateInfo still stores, because structural
    // validation has nothing to object to. That is the split 4.0-B draws.
    g_hook_calls = 0;
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, info)));
    IFE_CHECK(g_hook_calls == 0);

    b::ValidationHooks hooks{};
    hooks.TILE_TABLE = &reject_tile_table;

    // Attached: consulted, and its verdict is returned.
    const auto rejected = b::store(f.data(), 0, info, &hooks);
    IFE_CHECK(g_hook_calls == 1);
    IFE_CHECK(!rejected);
    IFE_CHECK(rejected.code == b::Check::OUT_OF_BOUNDS);

    info.X_EXTENT = 32;
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, info, &hooks)));
    IFE_CHECK(g_hook_calls == 2);
}

}  // namespace

int main() {
    test_visit_path();
    test_reads_what_was_written();
    test_valid_file_passes();
    test_corruption_is_caught();
    test_truncation();
    test_wider_stride_is_read_not_rejected();
    test_large_blob_length_reads_full_u32();
    test_image_bytes_is_sum_not_product();
    test_annotation_groups_read_from_entries();
    test_generated_writers_round_trip();
    test_writers_stay_within_size_of();
    test_validation_hook_is_dispatched();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_blocks_tests: all checks passed\n");
    return 0;
}
