/**
 * @file ife_builder_fixture.hpp
 * @brief The one Builder-written slide the sweeps share: the damage sweep and
 *        the recovery census sweep flip every bit of it.
 */
#ifndef IFE_BUILDER_FIXTURE_HPP
#define IFE_BUILDER_FIXTURE_HPP

#include <vector>

#include "IFE_Builder.hpp"
#include "IrisFileExtension.hpp"

/// A Builder-written slide: a single-plane layer, a Z-stacked layer (every one
/// of its streams is framed, whatever `frames` says) with a null tile, an
/// image, attributes and an ICC profile, laid out tiles → image bytes → tile
/// table → ICC → images → attributes → metadata. Small, because the sweeps
/// flip every bit of it.
inline std::vector<Iris::BYTE> ife_builder_fixture(bool frames = true) {
    using namespace Iris;
    using namespace Iris::File;
    namespace b = ::Iris::File::blocks;
    namespace k = ::Iris::File::constants;
    const Builder builder = Builder::create({.capacity = Size{1} << 20, .tile_frames = frames});
    BuilderTileTableInfo table;
    table.encoding      = TILE_ENCODING_JPEG;
    table.format        = FORMAT_R8G8B8A8;
    table.extent.width  = 512;
    table.extent.height = 512;
    table.extent.layers = {LayerExtent{.xTiles = 1, .yTiles = 1, .scale = 0.5f},
                           LayerExtent{.xTiles = 2, .yTiles = 2, .scale = 1.0f}};
    table.planes = {1, 3};
    builder.set_tile_table(table);

    const std::vector<BYTE> stream(24, 0xCD);   // contents are not IFE's business
    builder.append_tile(0, 0, stream.data(), stream.size());
    builder.append_tile(1, 0, stream.data(), stream.size(), 3);
    builder.append_tile(1, 1, stream.data(), stream.size(), 2);
    builder.append_tile(1, 2, stream.data(), stream.size(), 1);
    builder.append_null_tile(1, 3);
    builder.append_image(AssociatedImageInfo{.imageLabel   = "label",
                                             .width        = 64,
                                             .height       = 32,
                                             .encoding     = IMAGE_ENCODING_JPEG,
                                             .sourceFormat = FORMAT_B8G8R8A8,
                                             .orientation  = ORIENTATION_90},
                         stream.data(), stream.size());

    // The caller composes the structure. The Builder reports what it placed
    // (`tile_offsets` / `image_entries`); the caller names each block and its
    // order on disk.
    const auto tiles       = builder.tile_offsets();
    const Offset tiles_at  = builder->append(b::TileOffsetsCreateInfo{.entries = tiles});
    std::vector<b::LayerExtentEntry> extents;
    extents.reserve(table.extent.layers.size());
    for (std::size_t l = 0; l < table.extent.layers.size(); ++l)
        extents.push_back({.X_TILES  = table.extent.layers[l].xTiles,
                           .Y_TILES  = table.extent.layers[l].yTiles,
                           .SCALE    = table.extent.layers[l].scale,
                           .Z_PLANES = table.planes[l]});
    const Offset extents_at = builder->append(b::LayerExtentsCreateInfo{.entries = extents});
    const Offset table_at   = builder->append(b::TileTableCreateInfo{
        .ENCODING             = static_cast<k::TileEncodings>(table.encoding),
        .FORMAT               = static_cast<k::PixelFormats>(table.format),
        .TILE_OFFSETS_OFFSET  = tiles_at,
        .LAYER_EXTENTS_OFFSET = extents_at,
        .X_EXTENT             = table.extent.width,
        .Y_EXTENT             = table.extent.height,
        .TILE_LENGTH          = table.tileLength});

    const std::vector<BYTE> icc(16, 0x11);
    const Offset icc_at    = builder->append(b::IccProfileCreateInfo{.bytes = icc.data(), .count = icc.size()});

    const auto   images    = builder.image_entries();
    const Offset images_at = builder->append(b::ImagesCreateInfo{.entries = images});

    Attributes attributes;
    attributes.type    = METADATA_I2S;
    attributes.version = 1;
    attributes["key"]  = u8"value";
    std::vector<b::AttributeSizeEntry> pairs;
    for (const auto& [key, value] : attributes)
        pairs.push_back({.key   = key,
                         .value = std::string(reinterpret_cast<const char*>(value.data()),
                                              value.size())});
    const Offset sizes_at = builder->append(b::AttributeSizesCreateInfo{.entries = pairs});
    const Offset bytes_at = builder->append(b::AttributeBytesCreateInfo{.entries = pairs});
    const Offset attributes_at = builder->append(b::AttributesCreateInfo{
        .FORMAT       = static_cast<k::MetadataFormats>(attributes.type),
        .VERSION      = attributes.version,
        .SIZES_OFFSET = sizes_at,
        .BYTES_OFFSET = bytes_at});

    const Offset metadata_at   = builder->append(b::MetadataCreateInfo{
        .ATTRIBUTES_OFFSET = attributes_at,
        .IMAGES_OFFSET     = images_at,
        .ICC_COLOR_OFFSET  = icc_at,
        .MICRONS_PIXEL     = 0.25f,
        .MICRONS_PLANE     = 1.5f});
    builder.finalize({.tileTable = table_at, .metadata = metadata_at});
    return {builder->base(), builder->base() + builder.head()};
}

#endif  // IFE_BUILDER_FIXTURE_HPP
