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

/// Collects what the layer says, so a test can assert the diagnostic cites its
/// section rather than merely that something failed.
std::string g_diagnostic;

void collect(const char* message, void* user) {
    ++*static_cast<int*>(user);
    g_diagnostic = message;
}

int g_reports = 0;

b::ValidationHooks attached() {
    b::ValidationHooks hooks = b::conformance_layer();   // a copy; the layer is shared
    hooks.diagnostic = &collect;
    hooks.user       = &g_reports;
    return hooks;
}

// A layer extent set that is structurally perfect and violates the spec: zero
// tiles in a layer, and a scale that does not increase.
const b::LayerExtentEntry GOOD[3] = {
    {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f},
    {.X_TILES = 4, .Y_TILES = 4, .SCALE = 2.0f},
    {.X_TILES = 8, .Y_TILES = 8, .SCALE = 4.0f},
};
const b::LayerExtentEntry ZERO_TILES[2] = {
    {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f},
    {.X_TILES = 0, .Y_TILES = 4, .SCALE = 2.0f},   // X_TILES shall be >= 1
};
const b::LayerExtentEntry FLAT_SCALE[3] = {
    {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f},
    {.X_TILES = 4, .Y_TILES = 4, .SCALE = 2.0f},
    {.X_TILES = 8, .Y_TILES = 8, .SCALE = 2.0f},   // shall strictly increase
};

std::vector<BYTE> buffer() { return std::vector<BYTE>(4096, 0); }

void test_detached_costs_nothing_and_enforces_nothing() {
    auto f = buffer();
    g_reports = 0;

    // Structurally valid, spec-violating. With no layer attached this stores
    // and reports success -- which is the point: conformance is not the
    // library's business unless an application asks for it.
    const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES, .count = 2};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, bad)));
    IFE_CHECK(g_reports == 0);

    // And the block really is readable: the violation is semantic, not structural.
    const b::LAYER_EXTENTS written{f.data(), 0, f.size(), b::VERSION_WRITTEN};
    IFE_CHECK(static_cast<bool>(written.validate()));
    IFE_CHECK(written.count() == 2);
    IFE_CHECK(written.entry(1).x_tiles() == 0);
}

void test_attached_enforces_a_range_clause() {
    auto f = buffer();
    auto hooks = attached();
    g_reports = 0;
    g_diagnostic.clear();

    const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES, .count = 2};
    const auto status = b::store(f.data(), 0, bad, &hooks);

    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "X_TILES") == 0);
    IFE_CHECK(g_reports == 1);
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
    g_reports = 0;
    g_diagnostic.clear();

    const b::LayerExtentsCreateInfo bad{.entries = FLAT_SCALE, .count = 3};
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
    g_reports = 0;
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
    g_reports = 0;
    table.ENCODING = k::TileEncodings::TILE_ENCODING_JPEG;
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, table, &hooks)));

    // Every declared member passes — AVIF in particular: no committed bytes
    // carry it (the snapshot is JPEG), so the conformance layer must accept
    // it on the specification's word, and the reserved IRIS value too.
    for (auto encoding : {k::TileEncodings::TILE_ENCODING_AVIF,
                          k::TileEncodings::TILE_ENCODING_IRIS}) {
        g_reports = 0;
        table.ENCODING = encoding;
        IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, table, &hooks)));
        IFE_CHECK(g_reports == 0);
    }
}

void test_conformant_input_passes_with_the_layer_attached() {
    auto f = buffer();
    auto hooks = attached();
    g_reports = 0;

    const b::LayerExtentsCreateInfo good{.entries = GOOD, .count = 3};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, good, &hooks)));
    IFE_CHECK(g_reports == 0);   // nothing to say about a conformant file
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
    g_reports   = 0;

    // The outer layer runs and delegates; the generated one still catches it.
    const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES, .count = 2};
    const auto status = b::store(f.data(), 0, bad, &tracing);
    IFE_CHECK(outer_calls == 1);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(g_reports == 1);
}

// FILE_HEADER's clauses are the two mandatory pointers, not an enum: a header
// that points nowhere is structurally writable but is not a valid file. Both
// branches (each pointer absent on its own) must fire.
void test_attached_enforces_file_header_clauses() {
    auto f = buffer();
    auto hooks = attached();
    g_reports = 0;
    g_diagnostic.clear();

    b::FileHeaderCreateInfo header{};
    header.TILE_TABLE_OFFSET = ::IFE::constants::NULL_OFFSET;
    header.METADATA_OFFSET   = 128;
    const auto status = b::store(f.data(), 0, header, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "TILE_TABLE_OFFSET") == 0);
    IFE_CHECK(g_reports == 1);
    IFE_CHECK(g_diagnostic.find("clause ife-file-header") != std::string::npos);

    g_reports = 0;
    g_diagnostic.clear();
    header.TILE_TABLE_OFFSET = 64;
    header.METADATA_OFFSET   = ::IFE::constants::NULL_OFFSET;
    const auto status2 = b::store(f.data(), 0, header, &hooks);
    IFE_CHECK(!status2);
    IFE_CHECK(status2.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status2.field, "METADATA_OFFSET") == 0);

    // Both present: the layer has nothing to say.
    g_reports = 0;
    header.METADATA_OFFSET = 128;
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, header, &hooks)));
    IFE_CHECK(g_reports == 0);
}

void test_attached_enforces_attribute_format_membership() {
    auto f = buffer();
    auto hooks = attached();
    g_reports = 0;
    g_diagnostic.clear();

    // A value in the field's width but not in its declared domain.
    const b::AttributesCreateInfo bad{.FORMAT = static_cast<k::MetadataFormats>(200)};
    const auto status = b::store(f.data(), 0, bad, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "FORMAT") == 0);
    IFE_CHECK(g_diagnostic.find("clause ife-attributes") != std::string::npos);

    g_reports = 0;
    const b::AttributesCreateInfo good{.FORMAT = k::MetadataFormats::METADATA_FREE_TEXT};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, good, &hooks)));
    IFE_CHECK(g_reports == 0);
}

void test_attached_enforces_image_encoding_membership() {
    auto f = buffer();
    auto hooks = attached();
    g_reports = 0;
    g_diagnostic.clear();

    // Designated initializers must follow declaration order, so the
    // BYTES_OFFSET/WIDTH/HEIGHT slots are named before ENCODING.
    const b::ImageEntry entries[1] = {{
        .BYTES_OFFSET = ::IFE::constants::NULL_OFFSET, .WIDTH = 0, .HEIGHT = 0,
        .ENCODING = static_cast<k::ImageEncodings>(200),
        .FORMAT = k::PixelFormats::FORMAT_R8G8B8A8, .ORIENTATION = 0,
    }};
    const b::ImagesCreateInfo bad{.entries = entries, .count = 1};
    const auto status = b::store(f.data(), 0, bad, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "ENCODING") == 0);
    IFE_CHECK(g_diagnostic.find("clause ife-images") != std::string::npos);

    g_reports = 0;
    const b::ImageEntry good_entries[1] = {{
        .BYTES_OFFSET = ::IFE::constants::NULL_OFFSET, .WIDTH = 0, .HEIGHT = 0,
        .ENCODING = k::ImageEncodings::IMAGE_ENCODING_JPEG,
        .FORMAT = k::PixelFormats::FORMAT_R8G8B8A8, .ORIENTATION = 0,
    }};
    const b::ImagesCreateInfo good{.entries = good_entries, .count = 1};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, good, &hooks)));
    IFE_CHECK(g_reports == 0);
}

void test_attached_enforces_annotation_type_membership() {
    auto f = buffer();
    auto hooks = attached();
    g_reports = 0;
    g_diagnostic.clear();

    const b::AnnotationEntry entries[1] = {{
        .IDENTIFIER = 1,
        .BYTES_OFFSET = ::IFE::constants::NULL_OFFSET,
        .FORMAT = static_cast<k::AnnotationTypes>(200),
    }};
    const b::AnnotationsCreateInfo bad{.entries = entries, .count = 1};
    const auto status = b::store(f.data(), 0, bad, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "FORMAT") == 0);
    IFE_CHECK(g_diagnostic.find("clause ife-annotations") != std::string::npos);

    g_reports = 0;
    const b::AnnotationEntry good_entries[1] = {{
        .IDENTIFIER = 1,
        .BYTES_OFFSET = ::IFE::constants::NULL_OFFSET,
        .FORMAT = k::AnnotationTypes::ANNOTATION_TEXT,
    }};
    const b::AnnotationsCreateInfo good{.entries = good_entries, .count = 1};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, good, &hooks)));
    IFE_CHECK(g_reports == 0);
}

void test_attached_enforces_clinical_encoding_membership() {
    auto f = buffer();
    auto hooks = attached();
    g_reports = 0;
    g_diagnostic.clear();

    const b::ClinicalMetadataCreateInfo bad{
        .ENCODING = static_cast<k::ClinicalEncodings>(200)};
    const auto status = b::store(f.data(), 0, bad, &hooks);
    IFE_CHECK(!status);
    IFE_CHECK(status.code == b::Check::CONFORMANCE);
    IFE_CHECK(std::strcmp(status.field, "ENCODING") == 0);
    IFE_CHECK(g_diagnostic.find("clause ife-clinical-metadata") != std::string::npos);

    g_reports = 0;
    const b::ClinicalMetadataCreateInfo good{
        .ENCODING = k::ClinicalEncodings::CLINICAL_FASTFHIR};
    IFE_CHECK(static_cast<bool>(b::store(f.data(), 0, good, &hooks)));
    IFE_CHECK(g_reports == 0);
}

}  // namespace

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

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_validation_tests: all checks passed\n");
    return 0;
}
