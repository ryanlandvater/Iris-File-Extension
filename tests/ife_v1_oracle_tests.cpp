/**
 * @file ife_v1_oracle_tests.cpp
 * @brief Bytes written by the SHIPPED encoder, read back through the generated layer.
 *
 * This closes the last gap in the read path, and it is the only check of its
 * kind. Every other gate compares
 * the generated layer against a *description* of the format:
 *
 *   - ife_wire_parity_tests   compares generated constants to v1's constants
 *   - ife_blocks_tests        reads a buffer the test itself laid out
 *   - --check                 compares generated output to a fresh render
 *
 * All three would pass if the generated layer read a field at the right offset
 * with the wrong width — a u40 loaded as a u64, an enum cast from the wrong
 * type, an f16 returned as raw bits. Two descriptions agreeing is not
 * correctness (lessonsFromIFE B1). The only thing that settles it is bytes
 * produced by the encoder that has been writing real slides, read back through
 * the new reader, and compared against the values that went in.
 *
 * So: nothing here writes a byte. v1's STORE_* functions do, from
 * IrisFileExtensionLib, and the generated handles read. Blocks are written
 * leaves-first because v1's writers validate the blocks they reference.
 *
 * This file dies with src/IrisCodecExtension.* — but not before.
 * It is the reason the hand-written layer has to outlive the writers.
 */
#include "IrisTypes.hpp"
#include "IrisCodecTypes.hpp"

#include "IrisCodecExtension.hpp"   // the oracle: v1 writers
#include "IFE_Blocks.hpp"           // under test: generated readers

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

namespace S = IrisCodec::Serialization;
namespace b = ::IFE::blocks;
namespace k = ::IFE::constants;

// The values that go in. Every assertion at the bottom compares a generated
// read against one of these -- never against v1's reader, which would just be
// two readers agreeing.
constexpr std::uint32_t REVISION      = 0x0BADF00Du;
constexpr std::uint32_t X_EXTENT      = 4096;
constexpr std::uint32_t Y_EXTENT      = 2048;
constexpr float         MICRONS       = 0.2467f;
constexpr float         MAGNIFICATION = 40.0f;

// Tile payload sizes for the complete-file test. v1 validates that every tile
// entry's offset+size lies inside the file, so these must address real bytes.
// Exercising the top bytes of a u40 would need a >4 GB file; that is what
// test_v1_packed_widths_at_full_width below is for.
constexpr std::uint32_t TILE0_SIZE = 300;
constexpr std::uint32_t TILE1_SIZE = 145;

const std::string ICC_PROFILE = "ICC-PROFILE-BYTES-not-really-a-profile";
const std::string IMAGE_TITLE = "label";

void test_v1_bytes_read_through_generated_layer() {
    using namespace IrisCodec;

    // ---- what v1 will be asked to encode -------------------------------- //
    Iris::LayerExtents extents = {
        {.xTiles = 16, .yTiles = 8,  .scale = 1.0f, .downsample = 4.0f},
        {.xTiles = 32, .yTiles = 16, .scale = 2.0f, .downsample = 2.0f},
        {.xTiles = 64, .yTiles = 32, .scale = 4.0f, .downsample = 1.0f},
    };
    // Filled in once the tile-data region has an address.
    Abstraction::TileTable::Layers layers = {
        {{.offset = 0, .size = TILE0_SIZE}, {.offset = 0, .size = TILE1_SIZE}},
    };

    std::vector<Iris::BYTE> stream(64, 0xAB);
    S::ImageBytesCreateInfo image_bytes{};
    image_bytes.title     = IMAGE_TITLE;
    image_bytes.dataBytes = stream.size();
    // `data` is `BYTE* const`, so it must be set at construction.
    const S::ImageBytesCreateInfo image_bytes_ci{
        .offset = 0, .title = IMAGE_TITLE, .data = stream.data(), .dataBytes = stream.size()};

    // ---- lay the file out using v1's own size arithmetic ----------------- //
    // Deliberately v1's SIZE_*: the oracle decides how big its own blocks are.
    IrisCodec::Offset at = 0;
    auto place = [&at](IrisCodec::Size bytes) { const auto here = at; at += bytes; return here; };

    const IrisCodec::Offset header_at   = place(S::FILE_HEADER::HEADER_SIZE);
    const IrisCodec::Offset table_at    = place(S::TILE_TABLE::HEADER_SIZE);
    const IrisCodec::Offset extents_at  = place(S::SIZE_EXTENTS(extents));
    const IrisCodec::Offset tiles_at    = place(S::SIZE_TILE_OFFSETS(layers));
    const IrisCodec::Offset icc_at      = place(S::SIZE_ICC_COLOR_PROFILE(ICC_PROFILE));
    const IrisCodec::Offset imgbytes_at = place(S::SIZE_IMAGES_BYTES(image_bytes_ci));

    S::AssociatedImageCreateInfo images{};
    images.images.push_back(S::AssociatedImageCreateInfo::Entry{
        .offset = imgbytes_at,
        .info   = {.imageLabel   = IMAGE_TITLE,
                   .width        = 512,
                   .height       = 256,
                   .encoding     = IrisCodec::IMAGE_ENCODING_JPEG,
                   .sourceFormat = Iris::FORMAT_R8G8B8A8,
                   .orientation  = IrisCodec::ORIENTATION_90}});
    const IrisCodec::Offset images_at = place(S::SIZE_IMAGES_ARRAY(images));
    const IrisCodec::Offset meta_at   = place(S::METADATA::HEADER_SIZE);

    // The tile pixel data itself: unframed, per decision 10, so it is simply a
    // region the tile entries address. v1 validates that they do.
    const IrisCodec::Offset tile0_at  = place(TILE0_SIZE);
    const IrisCodec::Offset tile1_at  = place(TILE1_SIZE);
    layers[0][0].offset = tile0_at;
    layers[0][1].offset = tile1_at;

    const IrisCodec::Size   file_size = at;

    std::vector<Iris::BYTE> f(file_size, 0);
    Iris::BYTE* p = f.data();

    // ---- v1 writes, leaves first ----------------------------------------- //
    // v1's writers validate the blocks they point at, so a parent may only be
    // written once its children are on disk and valid.
    S::STORE_EXTENTS(p, extents_at, extents);
    S::STORE_TILE_OFFSETS(p, tiles_at, layers);
    S::STORE_TILE_TABLE(p, S::TileTableCreateInfo{
        .tileTableOffset    = table_at,
        .encoding           = IrisCodec::TILE_ENCODING_JPEG,
        .format             = Iris::FORMAT_R8G8B8A8,
        .cipherOffset       = S::NULL_OFFSET,
        .tilesOffset        = tiles_at,
        .layerExtentsOffset = extents_at,
        .layers             = static_cast<std::uint32_t>(extents.size()),
        .widthPixels        = X_EXTENT,
        .heightPixels       = Y_EXTENT});

    S::STORE_ICC_COLOR_PROFILE(p, icc_at, ICC_PROFILE);

    const S::ImageBytesCreateInfo stored_bytes{
        .offset = imgbytes_at, .title = IMAGE_TITLE,
        .data = stream.data(), .dataBytes = stream.size()};
    S::STORE_IMAGES_BYTES(p, stored_bytes);

    images.offset = images_at;
    S::STORE_IMAGES_ARRAY(p, images);

    S::STORE_METADATA(p, S::MetadataCreateInfo{
        .metadataOffset  = meta_at,
        .codecVersion    = {1, 2, 3},
        .attributes      = S::NULL_OFFSET,
        .images          = images_at,
        .ICC_profile     = icc_at,
        .annotations     = S::NULL_OFFSET,
        .micronsPerPixel = MICRONS,
        .magnification   = MAGNIFICATION});

    S::STORE_FILE_HEADER(p, S::HeaderCreateInfo{
        .fileSize        = file_size,
        .revision        = REVISION,
        .tileTableOffset = table_at,
        .metadataOffset  = meta_at});

    // ---- the generated layer reads what v1 wrote -------------------------- //
    const b::FILE_HEADER root{p, header_at, file_size, b::VERSION_WRITTEN};

    IFE_CHECK(static_cast<bool>(root));
    const auto deep = root.validate_deep();
    IFE_CHECK(static_cast<bool>(deep));
    if (!deep)
        std::fprintf(stderr, "  deep validation of a v1-written file failed in %s.%s\n",
                     deep.block, deep.field);

    IFE_CHECK(root.file_size() == file_size);
    IFE_CHECK(root.file_revision() == REVISION);
    IFE_CHECK(root.extension_major() == 1);
    IFE_CHECK(root.extension_minor() == 0);

    const auto tt = root.tile_table_offset();
    IFE_CHECK(tt.encoding() == k::TileEncodings::TILE_ENCODING_JPEG);
    IFE_CHECK(tt.format() == k::PixelFormats::FORMAT_R8G8B8A8);
    IFE_CHECK(tt.x_extent() == X_EXTENT);
    IFE_CHECK(tt.y_extent() == Y_EXTENT);
    IFE_CHECK(!static_cast<bool>(tt.cipher_offset()));   // nullable, absent

    // f32 written by v1, read by the generated layer.
    const auto le = tt.layer_extents_offset();
    IFE_CHECK(le.count() == extents.size());
    for (std::uint32_t i = 0; i < extents.size(); ++i) {
        IFE_CHECK(le.entry(i).x_tiles() == extents[i].xTiles);
        IFE_CHECK(le.entry(i).y_tiles() == extents[i].yTiles);
        IFE_CHECK(le.entry(i).scale()   == extents[i].scale);
    }

    // The packed widths. v1 stored these through STORE_U40 / STORE_U24; if the
    // generated reader loaded 8 or 4 bytes and masked, or read the wrong
    // width entirely, this is where it shows.
    const auto to = tt.tile_offsets_offset();
    IFE_CHECK(to.count() == 2);
    IFE_CHECK(to.entry(0).offset()     == tile0_at);
    IFE_CHECK(to.entry(0).size_field() == TILE0_SIZE);
    IFE_CHECK(to.entry(1).offset()     == tile1_at);
    IFE_CHECK(to.entry(1).size_field() == TILE1_SIZE);

    const auto md = root.metadata_offset();
    IFE_CHECK(md.codec_major() == 1);
    IFE_CHECK(md.codec_minor() == 2);
    IFE_CHECK(md.codec_build() == 3);
    IFE_CHECK(md.microns_pixel() == MICRONS);
    IFE_CHECK(md.magnification() == MAGNIFICATION);
    IFE_CHECK(!static_cast<bool>(md.attributes_offset()));
    IFE_CHECK(!static_cast<bool>(md.annotations_offset()));

    // A blob, sized by a u32 length v1 wrote.
    const auto icc = md.icc_color_offset().bytes();
    IFE_CHECK(icc.size == ICC_PROFILE.size());
    IFE_CHECK(std::memcmp(icc.data, ICC_PROFILE.data(), ICC_PROFILE.size()) == 0);

    const auto im = md.images_offset();
    IFE_CHECK(im.count() == 1);
    IFE_CHECK(im.entry(0).width()  == 512);
    IFE_CHECK(im.entry(0).height() == 256);
    IFE_CHECK(im.entry(0).encoding() == k::ImageEncodings::IMAGE_ENCODING_JPEG);
    IFE_CHECK(im.entry(0).format()   == k::PixelFormats::FORMAT_R8G8B8A8);

    // The sharpest single assertion in this file. v1 stored ORIENTATION_90 as
    // the raw binary16 pattern 0x55A0 (IrisCodecExtension.cpp STORE_U16); the
    // generated accessor decodes it. If load_f16 were wrong, or if the field
    // were read as a u16, this is 21920 rather than 90 -- and no other gate in
    // the project would notice.
    IFE_CHECK(im.entry(0).orientation() == 90.0f);

    const auto ib = im.entry(0).bytes_offset();
    IFE_CHECK(ib.title_size() == IMAGE_TITLE.size());
    IFE_CHECK(ib.image_size() == stream.size());
}

// The packed widths at their full width, from v1's own STORE_TILE_OFFSETS.
//
// The complete-file test above cannot reach them: v1 requires every tile entry
// to address bytes inside the file, so a 5-byte offset would need a file over
// 4 GB. Here the array is written on its own and read through the generated
// handle directly, which is legal -- STORE_TILE_OFFSETS writes; it is
// STORE_FILE_HEADER that performs the whole-file validation.
//
// This is the single most valuable assertion in the file. A u40 read as a u64,
// or a u24 read as a u32, is invisible to the wire-parity wall (offsets match
// either way), invisible to the synthetic block tests (which write through the
// same primitives they read), and invisible to --check. Only bytes from the
// shipped encoder settle it.
void test_v1_packed_widths_at_full_width() {
    using namespace IrisCodec;

    // Clear of NULL_TILE (0xFF'FFFF'FFFF) and NULL_ID (0xFF'FFFF) so these are
    // ordinary values rather than sentinels, but with every byte significant.
    constexpr std::uint64_t OFFSET_FULL = 0x000000FFFFFFFFFEull;  // 5 bytes, all set
    // (SIZE_MAX / OFFSET_MAX would collide with the <cstdint> macros.)
    constexpr std::uint32_t SIZE_FULL   = 0x00FFFFFEu;            // 3 bytes, all set
    constexpr std::uint64_t OFFSET_MID = 0x000000FEDCBA9876ull;
    constexpr std::uint32_t SIZE_MID   = 0x00ABCDEFu;

    const Abstraction::TileTable::Layers layers = {
        {{.offset = OFFSET_FULL, .size = SIZE_FULL},
         {.offset = OFFSET_MID, .size = SIZE_MID}},
    };

    std::vector<Iris::BYTE> f(S::SIZE_TILE_OFFSETS(layers), 0);
    S::STORE_TILE_OFFSETS(f.data(), 0, layers);

    const b::TILE_OFFSETS to{f.data(), 0, f.size(), b::VERSION_WRITTEN};
    IFE_CHECK(static_cast<bool>(to.validate()));
    IFE_CHECK(to.count() == 2);
    IFE_CHECK(to.stride() == S::TILE_OFFSET::SIZE);

    IFE_CHECK(to.entry(0).offset()     == OFFSET_FULL);
    IFE_CHECK(to.entry(0).size_field() == SIZE_FULL);
    IFE_CHECK(to.entry(1).offset()     == OFFSET_MID);
    IFE_CHECK(to.entry(1).size_field() == SIZE_MID);

    // A packed field must not bleed into its neighbour: the u24 SIZE sits
    // immediately after the u40 OFFSET, so a 4-byte store or load of either
    // corrupts the other. Reading both back intact is what proves the widths.
    IFE_CHECK((to.entry(0).offset() & ~0x000000FFFFFFFFFFull) == 0);
    IFE_CHECK((to.entry(0).size_field() & ~0x00FFFFFFu) == 0);
}

}  // namespace

int main() {
    test_v1_bytes_read_through_generated_layer();
    test_v1_packed_widths_at_full_width();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_v1_oracle_tests: all checks passed\n");
    return 0;
}
