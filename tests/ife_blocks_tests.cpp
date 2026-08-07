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
    ::IFE::store<std::uint64_t>(at(TILE_TABLE_AT, vt::TILE_TABLE::offset::VALIDATION), TILE_TABLE_AT);
    ::IFE::store<std::uint16_t>(at(TILE_TABLE_AT, vt::TILE_TABLE::offset::RECOVERY),
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
    ::IFE::store<std::uint64_t>(at(LAYER_EXTENTS_AT, vt::LAYER_EXTENTS::offset::VALIDATION), LAYER_EXTENTS_AT);
    ::IFE::store<std::uint16_t>(at(LAYER_EXTENTS_AT, vt::LAYER_EXTENTS::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_LAYER_EXTENTS));
    ::IFE::store<std::uint16_t>(at(LAYER_EXTENTS_AT, vt::LAYER_EXTENTS::offset::STRIDE),
                                vt::LAYER_EXTENTS::entry_size);
    ::IFE::store<std::uint32_t>(at(LAYER_EXTENTS_AT, vt::LAYER_EXTENTS::offset::COUNT), 2);
    for (std::uint32_t i = 0; i < 2; ++i) {
        BYTE* e = p + LAYER_EXTENTS_AT + vt::LAYER_EXTENTS::header_size + i * vt::LAYER_EXTENTS::entry_size;
        ::IFE::store<std::uint32_t>(e + vt::LAYER_EXTENTS::entry::offset::X_TILES, 8u << i);
        ::IFE::store<std::uint32_t>(e + vt::LAYER_EXTENTS::entry::offset::Y_TILES, 4u << i);
        ::IFE::store<float>(e + vt::LAYER_EXTENTS::entry::offset::SCALE, 1.0f * static_cast<float>(i + 1));
    }

    // ---- TILE_OFFSETS: one entry, exercising the packed widths ---------- //
    ::IFE::store<std::uint64_t>(at(TILE_OFFSETS_AT, vt::TILE_OFFSETS::offset::VALIDATION), TILE_OFFSETS_AT);
    ::IFE::store<std::uint16_t>(at(TILE_OFFSETS_AT, vt::TILE_OFFSETS::offset::RECOVERY),
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_TILE_OFFSETS));
    ::IFE::store<std::uint16_t>(at(TILE_OFFSETS_AT, vt::TILE_OFFSETS::offset::STRIDE),
                                vt::TILE_OFFSETS::entry_size);
    ::IFE::store<std::uint32_t>(at(TILE_OFFSETS_AT, vt::TILE_OFFSETS::offset::COUNT), 1);
    {
        BYTE* e = p + TILE_OFFSETS_AT + vt::TILE_OFFSETS::header_size;
        ::IFE::store_u40(e + vt::TILE_OFFSETS::entry::offset::OFFSET, 0xFEDCBA98ull);
        ::IFE::store_u24(e + vt::TILE_OFFSETS::entry::offset::SIZE, 0x00ABCDu);
    }

    // ---- METADATA: every optional offset absent ------------------------- //
    ::IFE::store<std::uint64_t>(at(METADATA_AT, vt::METADATA::offset::VALIDATION), METADATA_AT);
    ::IFE::store<std::uint16_t>(at(METADATA_AT, vt::METADATA::offset::RECOVERY),
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
            ::IFE::store<std::uint64_t>(f.data() + TILE_TABLE_AT + vt::TILE_TABLE::offset::VALIDATION, 99);
        }},
        {"array stride zero", b::Check::BAD_STRIDE, [](std::vector<BYTE>& f) {
            ::IFE::store<std::uint16_t>(f.data() + LAYER_EXTENTS_AT + vt::LAYER_EXTENTS::offset::STRIDE, 0);
        }},
        {"array count past EOF", b::Check::ARRAY_OVERRUN, [](std::vector<BYTE>& f) {
            ::IFE::store<std::uint32_t>(f.data() + LAYER_EXTENTS_AT + vt::LAYER_EXTENTS::offset::COUNT, 100000);
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
    ::IFE::store<std::uint64_t>(p + vt::LAYER_EXTENTS::offset::VALIDATION, LAYER_EXTENTS_AT);
    ::IFE::store<std::uint16_t>(p + vt::LAYER_EXTENTS::offset::RECOVERY,
                                static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_LAYER_EXTENTS));
    ::IFE::store<std::uint16_t>(p + vt::LAYER_EXTENTS::offset::STRIDE, WIDE);
    ::IFE::store<std::uint32_t>(p + vt::LAYER_EXTENTS::offset::COUNT, 2);
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

}  // namespace

int main() {
    test_visit_path();
    test_reads_what_was_written();
    test_valid_file_passes();
    test_corruption_is_caught();
    test_truncation();
    test_wider_stride_is_read_not_rejected();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_blocks_tests: all checks passed\n");
    return 0;
}
