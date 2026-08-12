/**
 * @file ife_v1_fixture.cpp
 * @brief The shipped encoder, isolated in its own translation unit.
 *
 * This is the only file in the runtime tests that includes the hand-written
 * layer. See ife_v1_fixture.hpp for why the separation is structural rather
 * than stylistic.
 */
#include "ife_v1_fixture.hpp"

#include "IrisTypes.hpp"
#include "IrisBuffer.hpp"   // Iris::Buffer, which v1's annotation writers take
#include "IrisCodecTypes.hpp"
#include "IrisCodecExtension.hpp"

#include <memory>

namespace v1_fixture {

namespace S = IrisCodec::Serialization;

std::vector<unsigned char> build_slide(Expected& expected) {
    using namespace IrisCodec;

    expected = expectations();

    Iris::LayerExtents extents = {
        {.xTiles = 2, .yTiles = 2, .scale = 1.0f, .downsample = 4.0f},
        {.xTiles = 4, .yTiles = 4, .scale = 2.0f, .downsample = 2.0f},
        {.xTiles = 8, .yTiles = 8, .scale = 4.0f, .downsample = 1.0f},
    };
    expected.layers = static_cast<std::uint32_t>(extents.size());

    // One tile entry per tile of every layer, flattened, as the format stores
    // them. Each addresses a real byte range: v1 validates that they do.
    std::uint32_t total = 0;
    for (const auto& e : extents) total += e.xTiles * e.yTiles;
    expected.tiles = total;

    Abstraction::TileTable::Layers layers(extents.size());
    for (std::size_t li = 0; li < extents.size(); ++li)
        layers[li].resize(static_cast<std::size_t>(extents[li].xTiles) * extents[li].yTiles);

    Attributes attributes;
    attributes.type = METADATA_FREE_TEXT;
    attributes[expected.attribute_key] =
        std::u8string(reinterpret_cast<const char8_t*>(expected.attribute_value.c_str()),
                      expected.attribute_value.size());

    std::vector<Iris::BYTE> stream(96, 0xAB);
    const S::ImageBytesCreateInfo image_bytes{
        .offset = 0, .title = expected.image_label,
        .data = stream.data(), .dataBytes = stream.size()};

    // ---- lay out with v1's own size arithmetic ---------------------------- //
    IrisCodec::Offset at = 0;
    auto place = [&at](IrisCodec::Size bytes) { const auto here = at; at += bytes; return here; };

    const auto header_at     = place(S::FILE_HEADER::HEADER_SIZE);
    const auto table_at      = place(S::TILE_TABLE::HEADER_SIZE);
    const auto extents_at    = place(S::SIZE_EXTENTS(extents));
    const auto tiles_at      = place(S::SIZE_TILE_OFFSETS(layers));
    const auto meta_at       = place(S::METADATA::HEADER_SIZE);
    const auto attributes_at = place(S::ATTRIBUTES::HEADER_SIZE);
    const auto attr_sizes_at = place(S::SIZE_ATTRIBUTES_SIZES(attributes));
    const auto attr_bytes_at = place(S::SIZE_ATTRIBUTES_BYTES(attributes));
    const auto icc_at        = place(S::SIZE_ICC_COLOR_PROFILE(expected.icc_profile));
    const auto imgbytes_at   = place(S::SIZE_IMAGES_BYTES(image_bytes));

    // Annotations: one per format. v1 sizes an ANNOTATION_BYTES block from the
    // Buffer its Annotation carries, so the payloads exist before the layout
    // runs, and each block is placed before the array that points at it.
    std::vector<Iris::Annotation> annotation_objects;
    annotation_objects.reserve(expected.annotations.size());
    for (const auto& spec : expected.annotations) {
        Iris::Annotation a{};
        a.type = static_cast<Iris::AnnotationTypes>(spec.format);
        a.data = std::make_shared<Iris::__INTERNAL__Buffer>(
            Iris::REFERENCE_STRONG, spec.payload.data(), spec.payload.size());
        annotation_objects.push_back(std::move(a));
    }

    S::AssociatedImageCreateInfo images{};
    images.images.push_back(S::AssociatedImageCreateInfo::Entry{
        .offset = imgbytes_at,
        .info   = {.imageLabel   = expected.image_label,
                   .width        = expected.image_width,
                   .height       = expected.image_height,
                   .encoding     = IMAGE_ENCODING_JPEG,
                   .sourceFormat = Iris::FORMAT_R8G8B8A8,
                   .orientation  = ORIENTATION_90}});
    const auto images_at = place(S::SIZE_IMAGES_ARRAY(images));

    std::vector<IrisCodec::Offset> annbytes_at;
    annbytes_at.reserve(annotation_objects.size());
    for (const auto& a : annotation_objects)
        annbytes_at.push_back(place(S::SIZE_ANNOTATION_BYTES(a)));

    S::AnnotationArrayCreateInfo annotations{};
    for (std::size_t i = 0; i < expected.annotations.size(); ++i) {
        const auto& spec = expected.annotations[i];
        annotations.annotations.insert({.identifier  = spec.identifier,
                                        .bytesOffset = annbytes_at[i],
                                        .type        = static_cast<Iris::AnnotationTypes>(spec.format),
                                        .xLocation   = spec.xLocation,
                                        .yLocation   = spec.yLocation,
                                        .xSize       = spec.xSize,
                                        .ySize       = spec.ySize,
                                        .width       = spec.width,
                                        .height      = spec.height,
                                        .parent      = spec.parent});
    }
    const auto annotations_at = place(S::SIZE_ANNOTATION_ARRAY(annotations));

    // Tile pixel data: unframed, simply a region the entries address.
    constexpr std::uint32_t TILE_BYTES = 16;
    for (auto& layer : layers)
        for (auto& tile : layer) {
            tile.offset = place(TILE_BYTES);
            tile.size   = TILE_BYTES;
        }

    expected.file_size     = at;
    expected.tile_table_at = table_at;
    expected.metadata_at   = meta_at;

    std::vector<unsigned char> f(expected.file_size, 0);
    Iris::BYTE* p = f.data();

    // ---- v1 writes, leaves first ------------------------------------------ //
    S::STORE_EXTENTS(p, extents_at, extents);
    S::STORE_TILE_OFFSETS(p, tiles_at, layers);
    S::STORE_TILE_TABLE(p, S::TileTableCreateInfo{
        .tileTableOffset    = table_at,
        .encoding           = TILE_ENCODING_JPEG,
        .format             = Iris::FORMAT_R8G8B8A8,
        .cipherOffset       = S::NULL_OFFSET,
        .tilesOffset        = tiles_at,
        .layerExtentsOffset = extents_at,
        .layers             = expected.layers,
        .widthPixels        = expected.x_extent,
        .heightPixels       = expected.y_extent});

    S::STORE_ATTRIBUTES_SIZES(p, attr_sizes_at, attributes);
    S::STORE_ATTRIBUTES_BYTES(p, attr_bytes_at, attributes);
    S::STORE_ATTRIBUTES(p, S::AttributesCreateInfo{
        .attributesOffset = attributes_at,
        .type             = attributes.type,
        .version          = attributes.version,
        .sizes            = attr_sizes_at,
        .bytes            = attr_bytes_at});

    S::STORE_ICC_COLOR_PROFILE(p, icc_at, expected.icc_profile);
    S::STORE_IMAGES_BYTES(p, S::ImageBytesCreateInfo{
        .offset = imgbytes_at, .title = expected.image_label,
        .data = stream.data(), .dataBytes = stream.size()});
    images.offset = images_at;
    S::STORE_IMAGES_ARRAY(p, images);

    for (std::size_t i = 0; i < annotation_objects.size(); ++i)
        S::STORE_ANNOTATION_BYTES(p, annbytes_at[i], annotation_objects[i]);
    annotations.offset = annotations_at;
    S::STORE_ANNOTATION_ARRAY(p, annotations);

    S::STORE_METADATA(p, S::MetadataCreateInfo{
        .metadataOffset  = meta_at,
        .codecVersion    = {1, 2, 3},
        .attributes      = attributes_at,
        .images          = images_at,
        .ICC_profile     = icc_at,
        .annotations     = annotations_at,
        .micronsPerPixel = expected.microns,
        .magnification   = expected.magnification});

    S::STORE_FILE_HEADER(p, S::HeaderCreateInfo{
        .fileSize        = expected.file_size,
        .revision        = expected.revision,
        .tileTableOffset = table_at,
        .metadataOffset  = meta_at});

    (void)header_at;
    return f;
}

}  // namespace v1_fixture
