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

/// A slide in the Builder's own layout: a single-plane layer, a Z-stacked
/// layer (every one of its streams is framed, whatever `frames` says) with a
/// null tile, an image, attributes and an ICC profile. Small, because the
/// sweeps flip every bit of it.
inline std::vector<Iris::BYTE> ife_builder_fixture(bool frames = true) {
    using namespace Iris;
    using namespace Iris::File;
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

    BuilderFinalizeInfo info;
    info.metadata.attributes.type    = METADATA_I2S;
    info.metadata.attributes.version = 1;
    info.metadata.attributes["key"]  = u8"value";
    info.metadata.ICC_profile.assign(16, '\x11');
    info.metadata.micronsPerPixel = 0.25f;
    info.micronsPerPlane          = 1.5f;
    builder.finalize(info);
    return {builder->base(), builder->base() + builder.head()};
}

#endif  // IFE_BUILDER_FIXTURE_HPP
