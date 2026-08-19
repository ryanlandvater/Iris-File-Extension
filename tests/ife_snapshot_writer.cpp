/**
 * @file ife_snapshot_writer.cpp
 * @brief Writes the corpus snapshot with the generated block writers.
 *
 * **Read this before assuming what the snapshot proves.** Its predecessor was
 * written by the shipped v1 encoder, which made it an *independent witness*:
 * the generated readers were checked against bytes a different implementation
 * produced, and that is the only kind of check that cannot be fooled by a
 * reader and writer agreeing with each other about the wrong thing.
 *
 * That property is spent. The 1.0 correction that gave ATTRIBUTE_SIZE its
 * KIND byte took the entry from six bytes to seven, so a stride of six is no
 * longer conformant and the old snapshot is rejected — correctly — by the
 * array validator. v1's writers were deleted in Phase 6, so nothing can
 * produce a corrected file except the generated layer itself. The snapshot is
 * therefore now a *round-trip pin*: it fixes the bytes this implementation
 * produces so that an unintended change to them is loud, and it is no longer
 * evidence about anyone else's encoder.
 *
 * What still holds: the digest in tests/corpus/manifest.json means a schema
 * edit that moves a shipped field breaks reading a file nobody regenerated,
 * which is most of what the oracle was for. What is gone until a second
 * implementation exists: the cross-check.
 *
 * The content is deliberately identical to what v1 wrote — same extents, same
 * attribute pair, same four annotations, same ICC bytes — because
 * tests/ife_v1_fixture.hpp states those values and every assertion in the
 * oracle and runtime tests reads them from there. This writer reproduces the
 * fixture's expectations; it does not define them.
 *
 * Usage: ife_snapshot_writer <output-path>
 */
#include "IFE_Blocks.hpp"
#include "ife_v1_fixture.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace b = ::IFE::blocks;
namespace k = ::IFE::constants;

using ::IFE::BYTE;
using ::IFE::Offset;
using ::IFE::Size;

/// Every tile in the fixture addresses this many bytes of (dummy) pixel data.
/// Real bytes rather than zero length because the entries must address a range
/// inside the file for deep validation to pass.
constexpr std::uint32_t TILE_BYTES = 16;

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output-path>\n", argv[0]);
        return 2;
    }

    v1_fixture::Expected expected = v1_fixture::expectations();

    // ---- the semantic content, from the fixture's own declarations -------- //
    const std::vector<b::LayerExtentEntry> extents = {
        {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f},
        {.X_TILES = 4, .Y_TILES = 4, .SCALE = 2.0f},
        {.X_TILES = 8, .Y_TILES = 8, .SCALE = 4.0f},
    };
    std::uint32_t tile_count = 0;
    for (const auto& e : extents) tile_count += e.X_TILES * e.Y_TILES;

    const std::vector<BYTE> image_stream(96, 0xAB);
    const std::string& icc = expected.icc_profile;

    // ---- lay the file out, leaves first ----------------------------------- //
    // Every offset is decided here, before a single byte is written: a
    // generated store() writes one block and never places another, so
    // placement is the caller's job in full.
    Offset at = 0;
    auto place = [&at](Size bytes) { const Offset here = at; at += bytes; return here; };

    b::LayerExtentsCreateInfo extents_info{.entries = extents};
    std::vector<b::TileOffsetEntry> tiles(tile_count);
    b::TileOffsetsCreateInfo tiles_info{.entries = tiles};
    b::IccProfileCreateInfo icc_info{
        .bytes = reinterpret_cast<const BYTE*>(icc.data()), .count = icc.size()};
    b::ImageBytesCreateInfo image_bytes_info{
        .TITLE_SIZE = static_cast<std::uint16_t>(expected.image_label.size()),
        .IMAGE_SIZE = static_cast<std::uint32_t>(image_stream.size())};

    const Offset header_at     = place(b::FILE_HEADER::header_size);
    const Offset table_at      = place(b::TILE_TABLE::header_size);
    const Offset extents_at    = place(b::size_of(extents_info));
    const Offset tiles_at      = place(b::size_of(tiles_info));
    const Offset meta_at       = place(b::METADATA::header_size);
    const Offset attributes_at = place(b::ATTRIBUTES::header_size);

    // ---- the nested sequences, placed before the root that names them ----- //
    // Each item is a complete attributes structure -- its own header, sizes
    // array and byte run -- so a sequence of N items is 3N blocks. They are
    // laid down first because the root's byte run carries their offsets, and
    // an offset has to exist before it can be written; nothing requires that
    // order on disk, since the offsets are absolute.
    struct Item {
        std::vector<b::AttributeSizeEntry> pairs;
        Offset sizes_at = 0, bytes_at = 0, attrs_at = 0;
    };
    std::vector<Item> items;
    std::vector<std::vector<Offset>> sequence_offsets;

    for (const auto& sequence : expected.nested_attributes) {
        std::vector<Offset> offsets;
        for (const auto& item_pairs : sequence.items) {
            Item item;
            for (const auto& [key, value] : item_pairs)
                item.pairs.push_back({.key = key, .value = value});
            items.push_back(std::move(item));
            Item& placed = items.back();
            placed.sizes_at = place(b::size_of(
                b::AttributeSizesCreateInfo{.entries = placed.pairs}));
            placed.bytes_at = place(b::size_of(
                b::AttributeBytesCreateInfo{.entries = placed.pairs}));
            placed.attrs_at = place(b::ATTRIBUTES::header_size);
            offsets.push_back(placed.attrs_at);
        }
        sequence_offsets.push_back(std::move(offsets));
    }

    // The root's attributes: one text value, then one entry per sequence
    // carrying the offsets of that sequence's items.
    std::vector<b::AttributeSizeEntry> attributes = {
        {.key = expected.attribute_key, .value = expected.attribute_value}};
    for (std::size_t i = 0; i < expected.nested_attributes.size(); ++i)
        attributes.push_back({.key    = expected.nested_attributes[i].key,
                              .nested = sequence_offsets[i],
                              .KIND   = k::AttributeKinds::ATTRIBUTE_NESTED});

    b::AttributeSizesCreateInfo attr_sizes_info{.entries = attributes};
    b::AttributeBytesCreateInfo attr_bytes_info{.entries = attributes};
    const Offset attr_sizes_at = place(b::size_of(attr_sizes_info));
    const Offset attr_bytes_at = place(b::size_of(attr_bytes_info));
    const Offset icc_at        = place(b::size_of(icc_info));
    // IMAGE_BYTES is a block with a trailing payload the schema does not
    // describe -- the label then the stream -- so its size_of covers the
    // header alone and the payload is placed here.
    const Offset imgbytes_at   = place(b::size_of(image_bytes_info)
                                     + expected.image_label.size() + image_stream.size());

    std::vector<b::ImageEntry> images = {
        {.BYTES_OFFSET = imgbytes_at,
         .WIDTH        = expected.image_width,
         .HEIGHT       = expected.image_height,
         .ENCODING     = k::ImageEncodings::IMAGE_ENCODING_JPEG,
         .FORMAT       = k::PixelFormats::FORMAT_R8G8B8A8,
         .ORIENTATION  = 90.0f}};
    b::ImagesCreateInfo images_info{.entries = images};
    const Offset images_at = place(b::size_of(images_info));

    // Each annotation's bytes are placed before the array that points at them.
    std::vector<Offset> annbytes_at;
    annbytes_at.reserve(expected.annotations.size());
    for (const auto& spec : expected.annotations) {
        const b::AnnotationBytesCreateInfo info{
            .bytes = reinterpret_cast<const BYTE*>(spec.payload.data()),
            .count = spec.payload.size()};
        annbytes_at.push_back(place(b::size_of(info)));
    }

    std::vector<b::AnnotationEntry> annotations;
    annotations.reserve(expected.annotations.size());
    for (std::size_t i = 0; i < expected.annotations.size(); ++i) {
        const auto& spec = expected.annotations[i];
        annotations.push_back({.IDENTIFIER   = spec.identifier,
                               .BYTES_OFFSET = annbytes_at[i],
                               .FORMAT       = static_cast<k::AnnotationTypes>(spec.format),
                               .X_LOCATION   = spec.xLocation,
                               .Y_LOCATION   = spec.yLocation,
                               .X_SIZE       = spec.xSize,
                               .Y_SIZE       = spec.ySize,
                               .PIXEL_WIDTH  = spec.width,
                               .PIXEL_HEIGHT = spec.height,
                               .PARENT_ID    = spec.parent});
    }
    // The named groups. Placed before the ANNOTATIONS block, which carries
    // the offsets of both arrays; 1.0 fields, so they belong in the 1.0
    // witness rather than waiting for a later version.
    std::vector<b::AnnotationGroupSizeEntry> group_sizes;
    std::vector<BYTE> group_bytes;
    for (const auto& group : expected.annotation_groups) {
        group_sizes.push_back(
            {.TITLE_SIZE   = static_cast<std::uint16_t>(group.title.size()),
             .MEMBER_COUNT = static_cast<std::uint32_t>(group.members.size())});
        group_bytes.insert(group_bytes.end(), group.title.begin(), group.title.end());
        // Identifiers are 24-bit, little-endian like everything else.
        for (const std::uint32_t id : group.members) {
            group_bytes.push_back(static_cast<BYTE>(id & 0xFF));
            group_bytes.push_back(static_cast<BYTE>((id >> 8) & 0xFF));
            group_bytes.push_back(static_cast<BYTE>((id >> 16) & 0xFF));
        }
    }
    b::AnnotationGroupSizesCreateInfo group_sizes_info{.entries = group_sizes};
    b::AnnotationGroupBytesCreateInfo group_bytes_info{
        .bytes = group_bytes.data(), .count = group_bytes.size()};
    const Offset group_sizes_at = place(b::size_of(group_sizes_info));
    const Offset group_bytes_at = place(b::size_of(group_bytes_info));

    b::AnnotationsCreateInfo annotations_info{
        .GROUP_SIZES_OFFSET = group_sizes_at,
        .GROUP_BYTES_OFFSET = group_bytes_at,
        .entries            = annotations};
    const Offset annotations_at = place(b::size_of(annotations_info));

    // Tile pixel data last: unframed, simply a region the entries address.
    for (auto& tile : tiles) {
        tile.OFFSET = place(TILE_BYTES);
        tile.SIZE   = TILE_BYTES;
    }

    const Size file_size = at;
    std::vector<BYTE> f(static_cast<std::size_t>(file_size), 0);
    BYTE* p = f.data();

    // ---- write ------------------------------------------------------------ //
    int failures = 0;
    auto wrote = [&failures](const char* what, const b::Status& status) {
        if (!status) {
            std::fprintf(stderr, "ife_snapshot_writer: %s failed (check %d at %llu)\n",
                         what, static_cast<int>(status.code),
                         static_cast<unsigned long long>(status.at));
            ++failures;
        }
    };

    wrote("LAYER_EXTENTS", b::store(p, extents_at, extents_info));
    wrote("TILE_OFFSETS",  b::store(p, tiles_at, tiles_info));
    wrote("TILE_TABLE",    b::store(p, table_at, b::TileTableCreateInfo{
        .ENCODING             = k::TileEncodings::TILE_ENCODING_JPEG,
        .FORMAT               = k::PixelFormats::FORMAT_R8G8B8A8,
        .TILE_OFFSETS_OFFSET  = tiles_at,
        .LAYER_EXTENTS_OFFSET = extents_at,
        .X_EXTENT             = expected.x_extent,
        .Y_EXTENT             = expected.y_extent}));

    // The sequence items first, each a complete structure in its own right.
    for (const auto& item : items) {
        wrote("nested ATTRIBUTE_SIZES", b::store(p, item.sizes_at,
            b::AttributeSizesCreateInfo{.entries = item.pairs}));
        wrote("nested ATTRIBUTE_BYTES", b::store(p, item.bytes_at,
            b::AttributeBytesCreateInfo{.entries = item.pairs}));
        wrote("nested ATTRIBUTES", b::store(p, item.attrs_at, b::AttributesCreateInfo{
            .FORMAT       = k::MetadataFormats::METADATA_DICOM,
            .VERSION      = 2024,
            .SIZES_OFFSET = item.sizes_at,
            .BYTES_OFFSET = item.bytes_at}));
    }

    wrote("ATTRIBUTE_SIZES", b::store(p, attr_sizes_at, attr_sizes_info));
    wrote("ATTRIBUTE_BYTES", b::store(p, attr_bytes_at, attr_bytes_info));
    wrote("ATTRIBUTES",      b::store(p, attributes_at, b::AttributesCreateInfo{
        .FORMAT       = k::MetadataFormats::METADATA_FREE_TEXT,
        .VERSION      = 0,
        .SIZES_OFFSET = attr_sizes_at,
        .BYTES_OFFSET = attr_bytes_at}));

    wrote("ICC_PROFILE", b::store(p, icc_at, icc_info));

    wrote("IMAGE_BYTES", b::store(p, imgbytes_at, image_bytes_info));
    // The label then the stream, in the bytes after the block header.
    BYTE* payload = p + imgbytes_at + b::IMAGE_BYTES::header_size;
    std::memcpy(payload, expected.image_label.data(), expected.image_label.size());
    std::memcpy(payload + expected.image_label.size(),
                image_stream.data(), image_stream.size());
    wrote("IMAGES", b::store(p, images_at, images_info));

    for (std::size_t i = 0; i < expected.annotations.size(); ++i) {
        const auto& spec = expected.annotations[i];
        wrote("ANNOTATION_BYTES", b::store(p, annbytes_at[i], b::AnnotationBytesCreateInfo{
            .bytes = reinterpret_cast<const BYTE*>(spec.payload.data()),
            .count = spec.payload.size()}));
    }
    wrote("ANNOTATION_GROUP_SIZES", b::store(p, group_sizes_at, group_sizes_info));
    wrote("ANNOTATION_GROUP_BYTES", b::store(p, group_bytes_at, group_bytes_info));
    wrote("ANNOTATIONS", b::store(p, annotations_at, annotations_info));

    wrote("METADATA", b::store(p, meta_at, b::MetadataCreateInfo{
        .CODEC_MAJOR        = 1,
        .CODEC_MINOR        = 2,
        .CODEC_BUILD        = 3,
        .ATTRIBUTES_OFFSET  = attributes_at,
        .IMAGES_OFFSET      = images_at,
        .ICC_COLOR_OFFSET   = icc_at,
        .ANNOTATIONS_OFFSET = annotations_at,
        .MICRONS_PIXEL      = expected.microns,
        .MAGNIFICATION      = expected.magnification}));

    // The root last, because it names the two blocks above and its FILE_SIZE
    // is the total only once everything has been placed.
    wrote("FILE_HEADER", b::store(p, header_at, b::FileHeaderCreateInfo{
        .FILE_SIZE         = file_size,
        .FILE_REVISION     = expected.revision,
        .TILE_TABLE_OFFSET = table_at,
        .METADATA_OFFSET   = meta_at}));

    // Tile pixel bytes: not a block, so nothing writes them but this.
    for (const auto& tile : tiles)
        std::memset(p + tile.OFFSET, 0xCD, tile.SIZE);

    if (failures) return 1;

    std::FILE* out = std::fopen(argv[1], "wb");
    if (!out) {
        std::fprintf(stderr, "ife_snapshot_writer: could not open %s\n", argv[1]);
        return 1;
    }
    const auto written = std::fwrite(f.data(), 1, f.size(), out);
    std::fclose(out);
    if (written != f.size()) {
        std::fprintf(stderr, "ife_snapshot_writer: short write to %s\n", argv[1]);
        return 1;
    }
    std::printf("ife_snapshot_writer: wrote %llu bytes to %s\n",
                static_cast<unsigned long long>(f.size()), argv[1]);
    return 0;
}
