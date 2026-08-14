/**
 * @file ife_v1_oracle_tests.cpp
 * @brief Bytes written by the SHIPPED encoder, read back through the generated layer.
 *
 * This closes the last gap in the read path, and it is the only check of its
 * kind. Every other gate compares the generated layer against a *description*
 * of the format:
 *
 *   - ife_blocks_tests       reads a buffer the test itself laid out
 *   - --check                compares generated output to a fresh render
 *
 * All would pass if the generated layer read a field at the right offset
 * with the wrong width — a u40 loaded as a u64, an enum cast from the wrong
 * type, an f16 returned as raw bits. Two descriptions agreeing is not
 * correctness (lessonsFromIFE B1). The only thing that settles it is bytes
 * produced by the encoder that has been writing real slides, read back
 * through the new reader, and compared against the values that went in.
 *
 * The bytes come from the snapshot: a whole slide the shipped encoder wrote,
 * hosted on iris.exampleslides.org, pinned by SHA-256 in
 * tests/corpus/manifest.json and fetched into .deps/corpus/ at configure
 * time. Nothing here writes a byte. The retired hand-written
 * layer that produced the snapshot is gone; the snapshot is what outlived it.
 */
#include "IFE_Blocks.hpp"
#include "ife_corpus_path.hpp"
#include "ife_v1_fixture.hpp"

#include <cstdio>
#include <cstring>
#include <string>
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
namespace b = ::IFE::blocks;
namespace k = ::IFE::constants;

/// Where the snapshot's LAYER_EXTENTS sits: v1 laid the file out head to
/// tail, header then tile table, so this is the sum of their 1.0 sizes —
/// the same arithmetic v1's place() did at encode time.
constexpr std::uint64_t EXTENTS_AT =
    b::FILE_HEADER::header_size_v1_0 + b::TILE_TABLE::header_size_v1_0;

std::vector<BYTE> read_whole_file(const std::string& __path) {
    std::FILE* in = std::fopen(__path.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "ife_v1_oracle_tests: could not open %s\n", __path.c_str());
        return {};
    }
    std::vector<BYTE> bytes;
    std::fseek(in, 0, SEEK_END);
    const long n = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    if (n > 0) bytes.resize(static_cast<std::size_t>(n));
    const auto read = std::fread(bytes.data(), 1, bytes.size(), in);
    std::fclose(in);
    if (read != bytes.size()) {
        std::fprintf(stderr, "ife_v1_oracle_tests: short read from %s\n", __path.c_str());
        return {};
    }
    return bytes;
}

// The complete snapshot, read through the generated layer. Every assertion
// compares a generated read against a value v1 was asked to encode (the
// fixture's expectations) -- never against v1's reader, which would just be
// two readers agreeing.
void test_v1_bytes_read_through_generated_layer(const std::vector<BYTE>& f,
                                                const v1_fixture::Expected& expected) {
    BYTE* p = const_cast<BYTE*>(f.data());

    // Bootstrap: the root's own version is unknowable until it has been read,
    // so it is constructed with every gate open for exactly one block, then
    // rebuilt at the version the file declares. This is what
    // IFE_Runtime::versioned_root does, and what a decoder must do -- reading
    // v1's bytes through a handle that claims this build's version is claiming
    // a version the file does not have.
    const b::FILE_HEADER bootstrap{p, 0, f.size(), UINT32_MAX};
    const std::uint32_t  declared =
        (static_cast<std::uint32_t>(bootstrap.extension_major()) << 16) |
        bootstrap.extension_minor();
    IFE_CHECK(declared == 0x00010000u);          // v1 writes 1.0
    IFE_CHECK(declared < b::VERSION_WRITTEN);    // and this build is newer

    // Everything below reads through `root`, at the declared version: the
    // traversal and the field reads alike. `newest` exists only for the two
    // assertions that are deliberately about a 1.1-compiled reader looking at
    // 1.0 bytes.
    const b::FILE_HEADER root{p, 0, f.size(), declared};
    const b::FILE_HEADER newest{p, 0, f.size(), b::VERSION_WRITTEN};

    IFE_CHECK(static_cast<bool>(root));

    // Deep validation follows the offset graph, so it must run at the declared
    // version. At a newer one the walk follows offset fields appended after
    // 1.0 that a 1.0 block does not contain -- METADATA's 1.1 CLINICAL offset
    // is still zero in a v1-written file, and the walk would follow it to byte
    // 0 and validate the file header as a clinical metadata block.
    const auto deep = root.validate_deep();
    IFE_CHECK(static_cast<bool>(deep));
    if (!deep)
        std::fprintf(stderr, "  deep validation of a v1-written file failed in %s.%s\n",
                     deep.block, deep.field);

    // New reader, old file: every 1.0 field reads identically through the
    // newest accessors, because append-only guarantees the 1.0 prefix never
    // moves. This is the property that lets a 1.1 build open a 1.0 slide.
    IFE_CHECK(newest.file_size()       == root.file_size());
    IFE_CHECK(newest.file_revision()   == root.file_revision());
    IFE_CHECK(newest.tile_table_offset().x_extent() == root.tile_table_offset().x_extent());

    IFE_CHECK(root.file_size() == expected.file_size);
    IFE_CHECK(root.file_revision() == expected.revision);
    IFE_CHECK(root.extension_major() == 1);
    IFE_CHECK(root.extension_minor() == 0);

    const auto tt = root.tile_table_offset();
    IFE_CHECK(static_cast<bool>(tt));
    IFE_CHECK(tt.encoding() == k::TileEncodings::TILE_ENCODING_JPEG);
    IFE_CHECK(tt.format() == k::PixelFormats::FORMAT_R8G8B8A8);
    IFE_CHECK(tt.x_extent() == expected.x_extent);
    IFE_CHECK(tt.y_extent() == expected.y_extent);
    IFE_CHECK(!static_cast<bool>(tt.cipher_offset()));   // nullable, absent

    // f32 written by v1, read by the generated layer.
    const auto le = tt.layer_extents_offset();
    IFE_CHECK(le.count() == expected.layers);
    constexpr std::uint32_t X_TILES[3] = {2, 4, 8};
    constexpr std::uint32_t Y_TILES[3] = {2, 4, 8};
    constexpr float         SCALES[3]  = {1.0f, 2.0f, 4.0f};
    for (std::uint32_t i = 0; i < expected.layers; ++i) {
        IFE_CHECK(le.entry(i).x_tiles() == X_TILES[i]);
        IFE_CHECK(le.entry(i).y_tiles() == Y_TILES[i]);
        IFE_CHECK(le.entry(i).scale()   == SCALES[i]);
    }

    // The packed widths. v1 stored these through STORE_U40 / STORE_U24; if the
    // generated reader loaded 8 or 4 bytes and masked, or read the wrong
    // width entirely, this is where it shows. The snapshot's 84 tile entries
    // are 16 bytes each, laid head to tail so the last one ends exactly at
    // EOF.
    const auto to = tt.tile_offsets_offset();
    IFE_CHECK(to.count() == expected.tiles);
    IFE_CHECK(to.stride() == b::TILE_OFFSETS::TILE_OFFSET::entry_size_v1_0);
    IFE_CHECK(to.entry(0).size_field() == 16);
    for (std::uint32_t i = 0; i + 1 < expected.tiles; ++i) {
        IFE_CHECK(to.entry(i).size_field() == 16);
        IFE_CHECK(to.entry(i + 1).offset() == to.entry(i).offset() + 16);
    }
    IFE_CHECK(to.entry(expected.tiles - 1).offset() + 16 == expected.file_size);

    // TILE_LENGTH, and the asymmetry that governs every appended header field.
    //
    // An appended *entry* field has two gates: the declared version and the
    // stride the array stores. An appended *block header* field has only the
    // first -- a block header records no size, so nothing cross-checks the
    // version.
    //
    // Both halves are asserted, because the second is the hazard. Read at the
    // declared version the field is correctly absent; read at this build's
    // version it is not absent but *wrong*, and wrong in the worst available
    // way. tile_length() gates on __version alone, and TILE_LENGTH sits at
    // offset 44 -- exactly where v1's 44-byte header ends. The next block
    // begins there, so the accessor returns the low two bytes of LAYER_EXTENTS'
    // VALIDATION word: not a sentinel, not zero, just a neighbouring block's
    // data wearing the name of a tile length.
    //
    // Nothing reports an error. That is why the version comes from the file,
    // and why nothing else in this test hangs off `newest`.
    IFE_CHECK(root.tile_table_offset().tile_length() == std::nullopt);
    IFE_CHECK(newest.tile_table_offset().tile_length() != std::nullopt);
    IFE_CHECK(newest.tile_table_offset().tile_length().value() ==
              static_cast<std::uint16_t>(EXTENTS_AT));

    const auto md = root.metadata_offset();
    IFE_CHECK(md.codec_major() == 1);
    IFE_CHECK(md.codec_minor() == 2);
    IFE_CHECK(md.codec_build() == 3);
    IFE_CHECK(md.microns_pixel() == expected.microns);
    IFE_CHECK(md.magnification() == expected.magnification);

    // Attributes: present in the snapshot (unlike the metadata of the
    // published example files), key-value pair sliced by the sizes array.
    const auto attrs = md.attributes_offset();
    IFE_CHECK(static_cast<bool>(attrs));
    // v1 defined METADATA_FREE_TEXT as an alias of METADATA_I2S, so the byte
    // it stored reads back as I2S under the schema's distinct value (errata
    // recorded in ife_fields.json; conformance claims are now unambiguous).
    IFE_CHECK(attrs.format() == k::MetadataFormats::METADATA_I2S);
    const auto attr_sizes = attrs.sizes_offset();
    IFE_CHECK(attr_sizes.count() == 1);
    IFE_CHECK(attr_sizes.entry(0).key_size()   == expected.attribute_key.size());
    IFE_CHECK(attr_sizes.entry(0).value_size() == expected.attribute_value.size());
    const auto attr_bytes = attrs.bytes_offset().bytes();
    IFE_CHECK(attr_bytes.size == expected.attribute_key.size() + expected.attribute_value.size());
    IFE_CHECK(std::memcmp(attr_bytes.data, expected.attribute_key.data(),
                          expected.attribute_key.size()) == 0);
    IFE_CHECK(std::memcmp(attr_bytes.data + expected.attribute_key.size(),
                          expected.attribute_value.data(), expected.attribute_value.size()) == 0);

    // A blob, sized by a u32 length v1 wrote.
    const auto icc = md.icc_color_offset().bytes();
    IFE_CHECK(icc.size == expected.icc_profile.size());
    IFE_CHECK(std::memcmp(icc.data, expected.icc_profile.data(), expected.icc_profile.size()) == 0);

    const auto im = md.images_offset();
    IFE_CHECK(im.count() == 1);
    IFE_CHECK(im.entry(0).width()  == expected.image_width);
    IFE_CHECK(im.entry(0).height() == expected.image_height);
    IFE_CHECK(im.entry(0).encoding() == k::ImageEncodings::IMAGE_ENCODING_JPEG);
    IFE_CHECK(im.entry(0).format()   == k::PixelFormats::FORMAT_R8G8B8A8);

    // The sharpest single assertion in this file. v1 stored ORIENTATION_90 as
    // the raw binary16 pattern 0x55A0 (v1's STORE_U16); the
    // generated accessor decodes it. If load_f16 were wrong, or if the field
    // were read as a u16, this is 21920 rather than 90 -- and no other gate in
    // the project would notice.
    IFE_CHECK(im.entry(0).orientation() == 90.0f);

    const auto ib = im.entry(0).bytes_offset();
    IFE_CHECK(ib.title_size() == expected.image_label.size());
    IFE_CHECK(ib.image_size() == 96);   // the fixture's 0xAB stream
    // The payload is not exposed as one span -- the schema does not describe
    // the title/stream split, only the two lengths above -- and deep
    // validation above already walked it in bounds.

    // ---- annotations ----------------------------------------------------- //
    // The four entries are asserted field by field, and it matters that they
    // disagree everywhere: reading an entry through the block header instead
    // of the entry pointer yields the same values twice, which only differing
    // siblings expose. ANNOTATION_ENTRY also puts BYTES_OFFSET at 3, behind a
    // 24-bit identifier, where an IMAGE_ENTRY puts it at 0 -- so a pointer
    // read through the image constant comes back shifted by 24 bits. One
    // annotation per annotation_types value; identifiers ascend (a std::set
    // on the v1 side), so entry(i) follows the fixture's expectations order.
    const auto an = md.annotations_offset();
    IFE_CHECK(static_cast<bool>(an));
    IFE_CHECK(an.count() == expected.annotations.size());
    for (std::size_t i = 0; i < expected.annotations.size(); ++i) {
        const auto& spec = expected.annotations[i];
        const auto e = an.entry(i);
        IFE_CHECK(e.identifier()   == spec.identifier);
        IFE_CHECK(e.format()       == static_cast<k::AnnotationTypes>(spec.format));
        IFE_CHECK(e.x_location()   == spec.xLocation);
        IFE_CHECK(e.y_location()   == spec.yLocation);
        IFE_CHECK(e.x_size()       == spec.xSize);
        IFE_CHECK(e.y_size()       == spec.ySize);
        IFE_CHECK(e.pixel_width()  == spec.width);
        IFE_CHECK(e.pixel_height() == spec.height);
        IFE_CHECK(e.parent_id()    == spec.parent);

        // Each entry's pointer resolves to its own payload, not a sibling's.
        const auto ab = e.bytes_offset().bytes();
        IFE_CHECK(ab.size == spec.payload.size());
        IFE_CHECK(std::memcmp(ab.data, spec.payload.data(), spec.payload.size()) == 0);
    }

    // No groups: v1 has no group writer, so the honest value is NULL_OFFSET,
    // and a reader must report absence rather than follow zero.
    IFE_CHECK(!static_cast<bool>(an.group_sizes_offset()));
    IFE_CHECK(an.group_sizes_offset().__offset == k::NULL_OFFSET);
    IFE_CHECK(!static_cast<bool>(an.group_bytes_offset()));
    IFE_CHECK(an.group_bytes_offset().__offset == k::NULL_OFFSET);
}

// A bare TILE_OFFSETS array whose u40/u24 fields have every byte significant.
// The full-width test could not build this through v1's writers inside this
// file any more (the writers are gone), so the bytes are the second hosted
// fixture: v1_tile_offsets_full_width.bin, pinned by digest like the snapshot.
//
// Clear of NULL_TILE (0xFF'FFFF'FFFF) and NULL_ID (0xFF'FFFF) so these are
// ordinary values rather than sentinels. The packed fields must not bleed
// into their neighbour: the u24 SIZE sits immediately after the u40 OFFSET,
// so a 4-byte store or load of either corrupts the other. Reading both back
// intact is what proves the widths.
void test_v1_packed_widths_at_full_width(const std::string& __corpus_dir) {
    constexpr std::uint64_t OFFSET_FULL = 0x000000FFFFFFFFFEull;  // 5 bytes, all set
    // (SIZE_MAX / OFFSET_MAX would collide with the <cstdint> macros.)
    constexpr std::uint32_t SIZE_FULL   = 0x00FFFFFEu;            // 3 bytes, all set
    constexpr std::uint64_t OFFSET_MID = 0x000000FEDCBA9876ull;
    constexpr std::uint32_t SIZE_MID   = 0x00ABCDEFu;

    const auto f = read_whole_file(__corpus_dir + "/v1_tile_offsets_full_width.bin");
    IFE_CHECK(f.size() == 32);   // ARRAY header + 2 * 8-byte entries
    if (f.size() != 32) return;

    const b::TILE_OFFSETS to{f.data(), 0, f.size(), b::VERSION_WRITTEN};
    IFE_CHECK(static_cast<bool>(to.validate()));
    IFE_CHECK(to.count() == 2);
    IFE_CHECK(to.stride() == b::TILE_OFFSETS::TILE_OFFSET::entry_size_v1_0);

    IFE_CHECK(to.entry(0).offset()     == OFFSET_FULL);
    IFE_CHECK(to.entry(0).size_field() == SIZE_FULL);
    IFE_CHECK(to.entry(1).offset()     == OFFSET_MID);
    IFE_CHECK(to.entry(1).size_field() == SIZE_MID);

    IFE_CHECK((to.entry(0).offset() & ~0x000000FFFFFFFFFFull) == 0);
    IFE_CHECK((to.entry(0).size_field() & ~0x00FFFFFFu) == 0);
}

// A 1.0 file read by a 1.1 decoder: the appended field must report absent.
//
// The version-gating tests prove this against a synthetic 200.0 spec, where
// both the file and the field are invented. This proves it against the real
// specification and bytes from the shipped 1.0 encoder -- the case that
// actually occurs when a 1.1 build opens a slide written before Z_PLANES
// existed. The snapshot's LAYER_EXTENTS is 1.0-written, and the whole file
// has been deep-validated above at the declared version.
//
// The version alone cannot decide it. VERSION_WRITTEN is 1.1 here, so the
// version gate is open and only the stride stored in v1's array -- 12 bytes,
// two short of Z_PLANES -- keeps the decoder from reading whatever follows
// the entry. That is precisely the guarantee <<ife-array-header>> makes to
// every future version, so it is worth an assertion of its own.
void test_v1_layer_extents_gate_the_1_1_plane_count(const std::vector<BYTE>& f) {
    BYTE* p = const_cast<BYTE*>(f.data());

    const b::LAYER_EXTENTS le{p, EXTENTS_AT, f.size(), b::VERSION_WRITTEN};
    IFE_CHECK(static_cast<bool>(le.validate()));

    // The 1.0 stride is what gates it; if the entry ever stops being 12 bytes
    // at 1.0 this test is measuring something else and should be revisited.
    IFE_CHECK(le.stride() == b::LAYER_EXTENTS::LAYER_EXTENT::entry_size_v1_0);
    IFE_CHECK(b::VERSION_WRITTEN >= 0x00010001u);

    IFE_CHECK(le.entry(0).z_planes() == std::nullopt);
    IFE_CHECK(le.entry(1).z_planes() == std::nullopt);
    IFE_CHECK(le.entry(2).z_planes() == std::nullopt);

    // The 1.0 fields stay readable across the gate -- gating the tail must not
    // disturb the prefix. The snapshot's extents are 2x2, 4x4, 8x8.
    IFE_CHECK(le.entry(0).x_tiles() == 2);
    IFE_CHECK(le.entry(1).y_tiles() == 4);
    IFE_CHECK(le.entry(2).x_tiles() == 8);
}

}  // namespace

int main(int argc, const char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 2;
    }
    // Bazel cannot pass a directory; BUILD.bazel passes the runfiles path of
    // one corpus file and its parent is the directory CTest passes directly.
    const std::string corpus_dir = ife_corpus_dir(argv[1]);

    v1_fixture::Expected expected;
    const auto bytes = v1_fixture::load_snapshot(corpus_dir + "/v1_snapshot.test_slide", expected);
    if (bytes.empty()) {
        std::fprintf(stderr, "ife_v1_oracle_tests: no snapshot in %s "
                             "(the corpus fetch runs at configure; see "
                             "tests/corpus/manifest.json)\n", corpus_dir.c_str());
        return 1;
    }

    test_v1_bytes_read_through_generated_layer(bytes, expected);
    test_v1_packed_widths_at_full_width(corpus_dir);
    test_v1_layer_extents_gate_the_1_1_plane_count(bytes);

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_v1_oracle_tests: all checks passed\n");
    return 0;
}
