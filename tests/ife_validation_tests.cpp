/**
 * @file ife_validation_tests.cpp
 * @brief The conformance layer, attached and detached.
 *
 * Validation is split in two, and the split is the thing under
 * test here:
 *
 *   - **Structural** validation is inline and mandatory. A block stores its own
 *     offset, carries its tag, and fits in the file. store() checks that always.
 *   - **Conformance** validation is optional and attachable. `X_TILES >= 1`,
 *     strictly increasing layer scale, enum membership: real requirements of
 *     the specification that say nothing about whether the bytes are readable.
 *
 * So there are two failure modes to guard, not one. A layer that is never
 * consulted is what a dispatch point silently degrades into; a check that fires
 * when *detached* would make "costs a null check" false and put spec policy in
 * everybody's hot path. Both are asserted below.
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IFE_Validation.hpp"

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

/// Where the layer writes its diagnostic, so a test can assert it cites its
/// section rather than merely that something failed. Cleared before each
/// store; non-empty afterwards means the layer reported.
std::string g_diagnostic;

b::ValidationHooks attached() {
    b::ValidationHooks hooks = b::conformance_layer();   // a copy; the layer is shared
    hooks.diagnostic = &g_diagnostic;
    return hooks;
}

// A layer extent set that is structurally perfect and violates the spec: zero
// tiles in a layer, and a scale that does not increase.
const std::vector<b::LayerExtentEntry> GOOD = {
    {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f},
    {.X_TILES = 4, .Y_TILES = 4, .SCALE = 2.0f},
    {.X_TILES = 8, .Y_TILES = 8, .SCALE = 4.0f},
};
const std::vector<b::LayerExtentEntry> ZERO_TILES = {
    {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f},
    {.X_TILES = 0, .Y_TILES = 4, .SCALE = 2.0f},   // X_TILES shall be >= 1
};
const std::vector<b::LayerExtentEntry> FLAT_SCALE = {
    {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f},
    {.X_TILES = 4, .Y_TILES = 4, .SCALE = 2.0f},
    {.X_TILES = 8, .Y_TILES = 8, .SCALE = 2.0f},   // shall strictly increase
};

std::vector<BYTE> buffer() { return std::vector<BYTE>(4096, 0); }

void test_detached_costs_nothing_and_enforces_nothing() {
    auto f = buffer();
    g_diagnostic.clear();

    // Structurally valid, spec-violating. With no layer attached this stores
    // and reports success -- which is the point: conformance is not the
    // library's business unless an application asks for it.
    const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, bad)));
    IFE_CHECK(g_diagnostic.empty());

    // And the block really is readable: the violation is semantic, not structural.
    const b::LAYER_EXTENTS written{f.data(), 0, f.size(), b::VERSION_WRITTEN};
    IFE_CHECK(static_cast<bool>(written.validate()));
    IFE_CHECK(written.count() == 2);
    IFE_CHECK(written.entry(1).x_tiles() == 0);
}

void test_attached_enforces_a_range_clause() {
    auto f = buffer();
    auto hooks = attached();
    g_diagnostic.clear();

    const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES};
    const auto status = b::store(f.data(), 0, bad, &hooks);

    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "X_TILES") == 0);
    IFE_CHECK(!g_diagnostic.empty());
    // Each diagnostic cites the stable clause id the specification anchors --
    // not a section number, which is positional and would point confidently at
    // the wrong requirement after a renumbering. `--validate` proves the id
    // resolves, so this string is a link a reader can actually follow.
    IFE_CHECK(g_diagnostic.find("X_TILES") != std::string::npos);
    IFE_CHECK(g_diagnostic.find("shall") != std::string::npos);
    IFE_CHECK(g_diagnostic.find("clause ife-layer-extents") != std::string::npos);
}

void test_attached_enforces_an_ordering_clause() {
    auto f = buffer();
    auto hooks = attached();
    g_diagnostic.clear();

    const b::LayerExtentsCreateInfo bad{.entries = FLAT_SCALE};
    const auto status = b::store(f.data(), 0, bad, &hooks);

    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(g_diagnostic.find("SCALE") != std::string::npos);
    IFE_CHECK(g_diagnostic.find("clause ife-layer-extents") != std::string::npos);

    // Ordering is a property of the sequence, so it cannot be caught one entry
    // at a time: every individual entry here is perfectly legal.
    for (const auto& entry : FLAT_SCALE) {
        IFE_CHECK(entry.X_TILES >= 1);
        IFE_CHECK(entry.Y_TILES >= 1);
    }
}

void test_attached_enforces_enum_membership() {
    auto f = buffer();
    auto hooks = attached();
    g_diagnostic.clear();

    // A value in the field's width but not in its declared domain.
    b::TileTableCreateInfo table{};
    table.ENCODING = static_cast<k::TileEncodings>(200);
    table.FORMAT   = k::PixelFormats::FORMAT_R8G8B8A8;

    const auto status = b::store(f.data(), 0, table, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(status.found == 200);
    IFE_CHECK(g_diagnostic.find("ENCODING") != std::string::npos);

    // A declared member passes.
    g_diagnostic.clear();
    table.ENCODING = k::TileEncodings::TILE_ENCODING_JPEG;
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, table, &hooks)));

    // Every declared member passes — AVIF in particular: no committed bytes
    // carry it (the snapshot is JPEG), so the conformance layer must accept
    // it on the specification's word, and the reserved IRIS value too.
    for (auto encoding : {k::TileEncodings::TILE_ENCODING_AVIF,
                          k::TileEncodings::TILE_ENCODING_IRIS}) {
        g_diagnostic.clear();
        table.ENCODING = encoding;
        IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, table, &hooks)));
        IFE_CHECK(g_diagnostic.empty());
    }
}

void test_conformant_input_passes_with_the_layer_attached() {
    auto f = buffer();
    auto hooks = attached();
    g_diagnostic.clear();

    const b::LayerExtentsCreateInfo good{.entries = GOOD};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, good, &hooks)));
    IFE_CHECK(g_diagnostic.empty());   // nothing to say about a conformant file
}

void test_a_block_without_clauses_is_untouched() {
    auto f = buffer();
    auto hooks = attached();

    // CIPHER is reserved and carries no normative clause, so the layer has no
    // hook for it: an unclaimed slot must stay null rather than becoming a
    // no-op function nobody notices.
    IFE_CHECK(b::conformance_layer().CIPHER == nullptr);
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, b::CipherCreateInfo{}, &hooks)));
}

void test_layers_chain() {
    auto f = buffer();
    static int outer_calls = 0;

    // A second layer in front of the generated one, as Vulkan's compose.
    b::ValidationHooks generated = attached();
    b::ValidationHooks tracing{};
    tracing.next = &generated;
    tracing.LAYER_EXTENTS = [](const b::LayerExtentsCreateInfo& info, ::IFE::Offset at,
                               const b::ValidationHooks* self) noexcept -> b::Status {
        ++outer_calls;
        if (self && self->next && self->next->LAYER_EXTENTS)
            return self->next->LAYER_EXTENTS(info, at, self->next);
        return {};
    };

    outer_calls = 0;
    g_diagnostic.clear();

    // The outer layer runs and delegates; the generated one still catches it,
    // writing through the sink attached to the generated copy.
    const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES};
    const auto status = b::store(f.data(), 0, bad, &tracing);
    IFE_CHECK(outer_calls == 1);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(!g_diagnostic.empty());
}

// FILE_HEADER's clauses are the two mandatory pointers, not an enum: a header
// that points nowhere is structurally writable but is not a valid file. Both
// branches (each pointer absent on its own) must fire.
void test_attached_enforces_file_header_clauses() {
    auto f = buffer();
    auto hooks = attached();
    g_diagnostic.clear();

    b::FileHeaderCreateInfo header{};
    header.TILE_TABLE_OFFSET = ::IFE::constants::NULL_OFFSET;
    header.METADATA_OFFSET   = 128;
    const auto status = b::store(f.data(), 0, header, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "TILE_TABLE_OFFSET") == 0);
    IFE_CHECK(!g_diagnostic.empty());
    IFE_CHECK(g_diagnostic.find("clause ife-file-header") != std::string::npos);

    g_diagnostic.clear();
    header.TILE_TABLE_OFFSET = 64;
    header.METADATA_OFFSET   = ::IFE::constants::NULL_OFFSET;
    const auto status2 = b::store(f.data(), 0, header, &hooks);
    IFE_CHECK(!status2);
    IFE_CHECK(status2.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status2.field, "METADATA_OFFSET") == 0);

    // Both present: the layer has nothing to say.
    g_diagnostic.clear();
    header.METADATA_OFFSET = 128;
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, header, &hooks)));
    IFE_CHECK(g_diagnostic.empty());
}

void test_attached_enforces_attribute_format_membership() {
    auto f = buffer();
    auto hooks = attached();
    g_diagnostic.clear();

    // A value in the field's width but not in its declared domain.
    const b::AttributesCreateInfo bad{.FORMAT = static_cast<k::MetadataFormats>(200)};
    const auto status = b::store(f.data(), 0, bad, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "FORMAT") == 0);
    IFE_CHECK(g_diagnostic.find("clause ife-attributes") != std::string::npos);

    g_diagnostic.clear();
    const b::AttributesCreateInfo good{.FORMAT = k::MetadataFormats::METADATA_FREE_TEXT};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, good, &hooks)));
    IFE_CHECK(g_diagnostic.empty());
}

void test_attached_enforces_image_encoding_membership() {
    auto f = buffer();
    auto hooks = attached();
    g_diagnostic.clear();

    // Designated initializers must follow declaration order, so the
    // BYTES_OFFSET/WIDTH/HEIGHT slots are named before ENCODING.
    const std::vector<b::ImageEntry> entries = {{
        .BYTES_OFFSET = ::IFE::constants::NULL_OFFSET, .WIDTH = 0, .HEIGHT = 0,
        .ENCODING = static_cast<k::ImageEncodings>(200),
        .FORMAT = k::PixelFormats::FORMAT_R8G8B8A8, .ORIENTATION = 0,
    }};
    const b::ImagesCreateInfo bad{.entries = entries};
    const auto status = b::store(f.data(), 0, bad, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "ENCODING") == 0);
    IFE_CHECK(g_diagnostic.find("clause ife-images") != std::string::npos);

    g_diagnostic.clear();
    const std::vector<b::ImageEntry> good_entries = {{
        .BYTES_OFFSET = ::IFE::constants::NULL_OFFSET, .WIDTH = 0, .HEIGHT = 0,
        .ENCODING = k::ImageEncodings::IMAGE_ENCODING_JPEG,
        .FORMAT = k::PixelFormats::FORMAT_R8G8B8A8, .ORIENTATION = 0,
    }};
    const b::ImagesCreateInfo good{.entries = good_entries};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, good, &hooks)));
    IFE_CHECK(g_diagnostic.empty());
}

void test_attached_enforces_annotation_type_membership() {
    auto f = buffer();
    auto hooks = attached();
    g_diagnostic.clear();

    const std::vector<b::AnnotationEntry> entries = {{
        .IDENTIFIER = 1,
        .BYTES_OFFSET = ::IFE::constants::NULL_OFFSET,
        .FORMAT = static_cast<k::AnnotationTypes>(200),
    }};
    const b::AnnotationsCreateInfo bad{.entries = entries};
    const auto status = b::store(f.data(), 0, bad, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "FORMAT") == 0);
    IFE_CHECK(g_diagnostic.find("clause ife-annotations") != std::string::npos);

    g_diagnostic.clear();
    const std::vector<b::AnnotationEntry> good_entries = {{
        .IDENTIFIER = 1,
        .BYTES_OFFSET = ::IFE::constants::NULL_OFFSET,
        .FORMAT = k::AnnotationTypes::ANNOTATION_TEXT,
    }};
    const b::AnnotationsCreateInfo good{.entries = good_entries};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, good, &hooks)));
    IFE_CHECK(g_diagnostic.empty());
}

void test_attached_enforces_clinical_encoding_membership() {
    auto f = buffer();
    auto hooks = attached();
    g_diagnostic.clear();

    const b::ClinicalMetadataCreateInfo bad{
        .ENCODING = static_cast<k::ClinicalEncodings>(200)};
    const auto status = b::store(f.data(), 0, bad, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "ENCODING") == 0);
    IFE_CHECK(g_diagnostic.find("clause ife-clinical-metadata") != std::string::npos);

    g_diagnostic.clear();
    const b::ClinicalMetadataCreateInfo good{
        .ENCODING = k::ClinicalEncodings::CLINICAL_FASTFHIR};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, good, &hooks)));
    IFE_CHECK(g_diagnostic.empty());
}

}  // namespace

// ---- the tile frame's anchor ------------------------------------------- //
// The frame is the one structure in the IFE laid out *backward*: its
// displacements are negative, measured from the first byte of the tile stream
// (the byte a TILE_OFFSETS entry names), so VALIDATION sits five bytes before
// that anchor and stores its own position. See CLAUDE.md for why.
//
// Two ways to get it wrong, and they fail differently. Both are pinned here
// because the layout is confusing relative to everything else in the format.

void test_frame_validation_is_anchored_five_bytes_before_the_stream() {
    auto f = buffer();
    constexpr ::IFE::Offset STREAM_AT = 512;   // what the tile table names

    IFE_CHECK(static_cast<bool>(b::store(f.data(), STREAM_AT,
        b::TilePixelDataCreateInfo{.TILE_INDEX = 7, .Z_PLANES = 0})));

    const b::TILE_PIXEL_DATA frame{f.data(), STREAM_AT, f.size(), b::VERSION_WRITTEN};
    IFE_CHECK(static_cast<bool>(frame.validate()));
    IFE_CHECK(frame.tile_index() == 7);

    // The contract, stated: the u40 at (anchor - 5) holds (anchor - 5).
    const auto stored = frame.validation();
    IFE_CHECK(stored.has_value());
    IFE_CHECK(*stored == STREAM_AT - 5);

    // The confusion this guards: storing the offset the TILE_OFFSETS entry
    // carries -- the stream's own address -- instead of the field's position.
    // Every other block in the format stores its own start, so this is the
    // mistake the layout invites.
    ::IFE::store_u40(f.data() + STREAM_AT - 5, STREAM_AT);
    const auto status = frame.validate();
    IFE_CHECK(!static_cast<bool>(status));
    IFE_CHECK(status.code == b::Check::BAD_VALIDATION);
}

void test_a_frame_written_from_its_own_start_fails_at_the_anchor() {
    auto f = buffer();
    constexpr ::IFE::Offset STREAM_AT = 512;
    const ::IFE::Offset frame_start = STREAM_AT - b::TILE_PIXEL_DATA::header_size;

    // The bug: passing the frame's START where store() wants the ANCHOR. It
    // writes a frame five bytes early that is entirely self-consistent -- a
    // u40 storing its own position is self-consistent wherever it lands -- so
    // nothing local to the frame objects.
    IFE_CHECK(static_cast<bool>(b::store(f.data(), frame_start,
        b::TilePixelDataCreateInfo{.TILE_INDEX = 7, .Z_PLANES = 0})));
    const b::TILE_PIXEL_DATA misplaced{f.data(), frame_start, f.size(), b::VERSION_WRITTEN};
    IFE_CHECK(static_cast<bool>(misplaced.validate()));   // self-consistent, and wrong

    // What catches it is validating at the offset the TILE_OFFSETS entry
    // actually names. There is no frame there, so the anchor check fails --
    // which is the check a reader performs anyway.
    const b::TILE_PIXEL_DATA at_anchor{f.data(), STREAM_AT, f.size(), b::VERSION_WRITTEN};
    IFE_CHECK(!static_cast<bool>(at_anchor.validate()));
}

int main() {
    test_detached_costs_nothing_and_enforces_nothing();
    test_attached_enforces_a_range_clause();
    test_attached_enforces_an_ordering_clause();
    test_attached_enforces_enum_membership();
    test_conformant_input_passes_with_the_layer_attached();
    test_a_block_without_clauses_is_untouched();
    test_layers_chain();
    test_attached_enforces_file_header_clauses();
    test_attached_enforces_attribute_format_membership();
    test_attached_enforces_image_encoding_membership();
    test_attached_enforces_annotation_type_membership();
    test_attached_enforces_clinical_encoding_membership();
    test_frame_validation_is_anchored_five_bytes_before_the_stream();
    test_a_frame_written_from_its_own_start_fails_at_the_anchor();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_validation_tests: all checks passed\n");
    return 0;
}
