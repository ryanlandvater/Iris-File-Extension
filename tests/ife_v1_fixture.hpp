/**
 * @file ife_v1_fixture.hpp
 * @brief The bytes the SHIPPED encoder wrote, for tests that read them back.
 *
 * Deliberately free of both layers' types — the generated runtime and the
 * retired hand-written layer both define IrisCodec::Abstraction, so a header
 * consumed from either side must not name either. The seam survives because
 * what crosses it is bytes, not types: v1 wrote the snapshot once; it is
 * hosted, pinned by digest in tests/corpus/manifest.json, and fetched into
 * .deps/corpus/ at configure time. Nothing but
 * `std::vector<unsigned char>` crosses between.
 */

#ifndef ife_v1_fixture_hpp
#define ife_v1_fixture_hpp

#include <cstdint>
#include <string>
#include <vector>

namespace v1_fixture {

/// One annotation the fixture encodes.
///
/// Plain types only, for the reason the file header gives: this is included by
/// a translation unit that cannot see either layer, so `format` carries the
/// annotation_types value rather than the enumeration.
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

/// 24-bit sentinel for "no parent". Spelled out because v1 declares its
/// NULL_ID as a non-static member and the runtime as a static one, so neither
/// spelling of Annotation::NULL_ID compiles against both.
inline constexpr std::uint32_t NULL_ANNOTATION_ID = 16777215U;

/// One named annotation group: a title, and the identifiers of the
/// annotations belonging to it. The group byte array carries the title
/// followed by one 24-bit identifier per member; the sizes entry is what
/// says where the title ends, exactly as the attribute arrays work.
struct AnnotationGroupSpec {
    std::string                title;
    std::vector<std::uint32_t> members;
};

/// One attribute whose value is a sequence of nested structures.
///
/// `items` is a sequence in the DICOM sense: each item is a complete set of
/// key/value pairs, carried on disk by its own attributes block. Plain types
/// only, for the reason the file header gives.
struct NestedAttributeSpec {
    std::string key;
    std::vector<std::vector<std::pair<std::string, std::string>>> items;
};

/// The values the fixture encodes, so a reader can assert against the inputs
/// rather than against another reader.
struct Expected {
    std::uint64_t file_size      = 0;
    std::uint32_t revision       = 0;
    std::uint32_t x_extent       = 0;
    std::uint32_t y_extent       = 0;
    float         microns        = 0.f;
    float         magnification  = 0.f;
    std::uint32_t layers         = 0;
    std::uint32_t tiles          = 0;   ///< total across all layers
    std::string   icc_profile;
    std::string   image_label;
    std::uint32_t image_width    = 0;
    std::uint32_t image_height   = 0;
    std::string   attribute_key;
    std::string   attribute_value;
    std::vector<NestedAttributeSpec> nested_attributes;
    std::vector<AnnotationSpec> annotations;
    std::vector<AnnotationGroupSpec> annotation_groups;
};

/// The values the fixture encodes. Pure data, so a test that cannot link the
/// hand-written layer still knows what to expect; `file_size` is filled in by
/// load_snapshot from the length of the fetched file.
inline Expected expectations() {
    Expected e;
    e.revision        = 0x00C0FFEEu;
    e.x_extent        = 2048;
    e.y_extent        = 1024;
    e.microns         = 0.2467f;
    e.magnification   = 40.0f;
    e.layers          = 3;
    e.tiles           = 2 * 2 + 4 * 4 + 8 * 8;
    e.icc_profile     = "ICC-PROFILE-BYTES";
    e.image_label     = "thumbnail";
    e.image_width     = 512;
    e.image_height    = 256;
    e.attribute_key   = "SCANNER";
    e.attribute_value = "TestCo";
    // Nested values, as DICOM actually shapes them: a sequence attribute whose
    // value is zero or more items, each item a data set of its own. Two items
    // in the first, because a single-item sequence does not prove the offset
    // run is walked at all; two pairs in the first item, because an item with
    // one pair does not prove its own slicing. The second sequence is empty --
    // legal in DICOM, and the case that distinguishes "a sequence of no items"
    // from "a text value of length zero", which is why KIND is a byte and not
    // an inference from the value's size.
    e.nested_attributes = {
        {.key   = "0048,0105",          // Optical Path Sequence
         .items = {{{"0008,0100", "SM"}, {"0008,0104", "Brightfield"}},
                   {{"0008,0100", "BF"}}}},
        {.key = "0040,0610", .items = {}},   // Specimen Preparation Sequence
    };
    // One annotation per annotation_types value, so every format the
    // specification defines appears in a slide the shipped encoder wrote.
    // They differ in every field on purpose: reading an entry through the
    // block header rather than the entry pointer decodes a single annotation
    // correctly by accident, and only differing siblings expose it.
    // Identifiers ascend because Metadata::AnnotationIDs is a std::set, so
    // this is also the order a consumer iterating it will see.
    e.annotations = {
        {.identifier = 0x000101, .format = 1,  // ANNOTATION_PNG
         .xLocation = 0.10f, .yLocation = 0.20f, .xSize = 0.05f,  .ySize = 0.025f,
         .width = 256, .height = 128, .parent = NULL_ANNOTATION_ID,
         .payload = std::string("\x89PNG\r\n\x1a\n", 8) + "not-a-real-png"},
        {.identifier = 0x000202, .format = 2,  // ANNOTATION_JPEG
         .xLocation = 0.30f, .yLocation = 0.40f, .xSize = 0.10f,  .ySize = 0.050f,
         .width = 128, .height = 64,  .parent = 0x000101,
         .payload = std::string("\xFF\xD8\xFF\xE0", 4) + "not-a-real-jpeg"},
        {.identifier = 0x000303, .format = 3,  // ANNOTATION_SVG
         .xLocation = 0.50f, .yLocation = 0.60f, .xSize = 0.20f,  .ySize = 0.100f,
         .width = 64,  .height = 32,  .parent = NULL_ANNOTATION_ID,
         .payload = "<svg viewBox='0 0 16 16'><rect width='16' height='16'/></svg>"},
        {.identifier = 0x000404, .format = 4,  // ANNOTATION_TEXT
         .xLocation = 0.70f, .yLocation = 0.80f, .xSize = 0.40f,  .ySize = 0.200f,
         .width = 32,  .height = 16,  .parent = 0x000303,
         .payload = "a plain-text annotation"},
    };
    // Two groups, of different title lengths and different member counts.
    // One group would not prove the byte run is sliced at all -- the reader
    // could return the whole run and look correct -- and equal-sized groups
    // would not prove the slice moves with the entry rather than by a fixed
    // stride. The members name annotations that exist above, so a consumer
    // resolving a group against the annotation set finds every one of them.
    e.annotation_groups = {
        {.title = "Tumor front", .members = {0x000101, 0x000202}},
        {.title = "QC",          .members = {0x000404}},
    };
    return e;
}

/// Load the snapshot file (`.deps/corpus/v1_0_witness.test_slide`) and the
/// values v1 encoded into it. Empty on any I/O failure — callers must treat
/// that as fatal, exactly as a missing corpus file is.
std::vector<unsigned char> load_snapshot(const std::string& __path, Expected& __expected);

}  // namespace v1_fixture

#endif  // ife_v1_fixture_hpp
