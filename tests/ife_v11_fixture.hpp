/**
 * @file ife_v11_fixture.hpp
 * @brief What the 1.1 witness contains, for tests that read it back.
 *
 * The 1.1 witness (v1_1_witness.test_slide) is the version's whole
 * structural surface: every 1.1 field at a non-default setting, so a reader
 * honours the field rather than assuming the default. It is written by
 * tests/ife_corpus_writer_11.cpp against the current generated layer,
 * hosted, pinned by digest in tests/corpus/manifest.json, and fetched into
 * .deps/corpus/ at configure time — the same delivery as the 1.0 snapshot.
 *
 * Plain types only, exactly as ife_v1_fixture.hpp does: enum values are
 * carried as their integers, and the test casts them at the read site. What
 * crosses the boundary is bytes and numbers, never layer types.
 */

#ifndef ife_v11_fixture_hpp
#define ife_v11_fixture_hpp

#include <cstdint>
#include <string>
#include <vector>

namespace v11_fixture {

inline constexpr std::uint32_t NULL_ANNOTATION_ID = 16777215U;   ///< 24-bit "no parent"
inline constexpr std::uint64_t NULL_TILE = 0xFF'FFFF'FFFFull;    ///< 40-bit "no tile"

/// One layer-extents entry, with the 1.1 plane count.
struct LayerSpec {
    std::uint32_t x_tiles  = 0;
    std::uint32_t y_tiles  = 0;
    float         scale    = 0.f;
    std::uint16_t z_planes = 1;   ///< 1.1 field; a 1.0 file stores none
};

/// One annotation the fixture encodes, as in the 1.0 pair.
struct AnnotationSpec {
    std::uint32_t identifier = 0;
    std::uint8_t  format     = 0;   ///< PNG 1, JPEG 2, SVG 3, TEXT 4
    float         xLocation  = 0.f;
    float         yLocation  = 0.f;
    float         xSize      = 0.f;
    float         ySize      = 0.f;
    std::uint32_t width      = 0;
    std::uint32_t height     = 0;
    std::uint32_t parent     = 0;
    std::string   payload;
};

/// One named annotation group, as in the 1.0 pair.
struct AnnotationGroupSpec {
    std::string                title;
    std::vector<std::uint32_t> members;
};

/// The values the fixture encodes, so a reader can assert against the inputs
/// rather than against another reader. Enum values are integers: TileEncodings
/// JPEG = 2, PixelFormats R8G8B8A8 = 4, MetadataFormats I2S = 1,
/// ImageEncodings JPEG = 2, AnnotationTypes TEXT = 4,
/// ClinicalEncodings HL7_V2 = 1. `file_size` is filled in by load_snapshot
/// from the length of the fetched file.
struct Expected {
    std::uint64_t file_size      = 0;
    std::uint32_t revision       = 0;
    std::uint32_t x_extent       = 0;
    std::uint32_t y_extent       = 0;
    /// 1.1 field. 128, deliberately not the 256 that zero and absence both
    /// mean — a decoder assuming the default passes every other gate.
    std::uint16_t tile_length    = 256;
    std::uint8_t  tile_encoding  = 0;
    std::uint8_t  tile_format    = 0;
    std::uint32_t tile_stream_bytes = 0;
    std::uint32_t tile_count     = 0;
    /// The sparse slot; the sentinel is exercised by exactly one. Set to
    /// tile_count when the fixture is dense.
    std::uint32_t null_tile_index = 0xFFFFFFFFu;
    /// Per non-null stream, in tile order. Zero denotes a single plane: the
    /// layer declares a maximum of 3, and an individual tile may carry fewer.
    std::vector<std::uint16_t> frame_z_planes;
    std::vector<LayerSpec>     layers;

    std::uint8_t  codec_major   = 0;
    std::uint8_t  codec_minor   = 0;
    std::uint8_t  codec_build   = 0;
    float         microns_pixel = 0.f;
    float         magnification = 0.f;
    /// 1.1 field; zero means not Z-stacked or not recorded.
    float         microns_plane = 0.f;
    std::uint8_t  clinical_encoding = 0;   ///< ClinicalEncodings value
    std::string   clinical_bytes;          ///< the stream's payload bytes

    std::string   attribute_key;
    std::string   attribute_value;
    std::uint8_t  attribute_format = 0;    ///< MetadataFormats value

    std::string   image_label;
    std::uint32_t image_width  = 0;
    std::uint32_t image_height = 0;
    std::uint8_t  image_encoding = 0;      ///< ImageEncodings value
    std::uint8_t  image_format   = 0;      ///< PixelFormats value
    float         image_orientation = 0.f;
    std::uint32_t image_stream_bytes = 0;
    std::uint32_t icc_bytes    = 0;

    AnnotationSpec      annotation;
    AnnotationGroupSpec group;
};

/// The values the 1.1 witness encodes. Pure data, so any test that links the
/// current generated layer knows what to expect without naming it.
inline Expected expectations() {
    Expected e;
    e.revision           = 7;
    e.x_extent           = 256;
    e.y_extent           = 256;
    e.tile_length        = 128;
    e.tile_encoding      = 2;    // TileEncodings::TILE_ENCODING_JPEG
    e.tile_format        = 4;    // PixelFormats::FORMAT_R8G8B8A8
    e.tile_stream_bytes  = 16;
    e.tile_count         = 4;    // one 2x2 layer
    e.null_tile_index    = 3;    // the last slot is the sparse one
    e.frame_z_planes     = {3, 0, 0};
    e.layers             = {{.x_tiles = 2, .y_tiles = 2, .scale = 1.0f, .z_planes = 3}};
    e.codec_major        = 1;
    e.codec_minor        = 1;
    e.codec_build        = 0;
    e.microns_pixel      = 0.25f;
    e.magnification      = 40.0f;
    e.microns_plane      = 0.5f;
    e.clinical_encoding  = 1;    // ClinicalEncodings::CLINICAL_HL7_V2
    e.clinical_bytes     = "MSH|^~\\&|IRIS|LAB|EMR|HOSP|20260818120000||ADT^A08|1|P|2.5\r";
    e.attribute_key      = "SeriesDescription";
    e.attribute_value    = "IFE 1.1 witness";
    e.attribute_format   = 1;    // MetadataFormats::METADATA_I2S
    e.image_label        = "thumbnail";
    e.image_width        = 64;
    e.image_height       = 64;
    e.image_encoding     = 2;    // ImageEncodings::IMAGE_ENCODING_JPEG
    e.image_format       = 4;    // PixelFormats::FORMAT_R8G8B8A8
    e.image_orientation  = 90.0f;
    e.image_stream_bytes = 48;
    e.icc_bytes          = 64;
    e.annotation         = {.identifier = 1,
                            .format     = 4,   // AnnotationTypes::ANNOTATION_TEXT
                            .xLocation  = 0.25f,
                            .yLocation  = 0.25f,
                            .xSize      = 0.5f,
                            .ySize      = 0.5f,
                            .width      = 32,
                            .height     = 32,
                            .parent     = NULL_ANNOTATION_ID,
                            .payload    = "SVG-ish annotation bytes"};
    e.group              = {.title = "Tumor front", .members = {1, 2}};
    return e;
}

/// Load the 1.1 witness file (`.deps/corpus/v1_1_witness.test_slide`) and the
/// expectations it encodes. Empty on failure — missing corpus is a hard
/// error for the caller, exactly as a missing 1.0 snapshot is.
std::vector<unsigned char> load_snapshot(const std::string& __path, Expected& __expected);

}  // namespace v11_fixture

#endif  // ife_v11_fixture_hpp
