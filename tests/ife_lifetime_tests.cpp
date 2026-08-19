/**
 * @file ife_lifetime_tests.cpp
 * @brief Views into the caller's mapping are valid while the mapping lives;
 *        nothing may view into a temporary.
 *
 * The abstraction reads over a lens of the caller's mapped bytes, exactly as
 * FastFHIR reads over its arena: the caller owns the mapping (a VMA/arena in
 * production, a plain buffer here) and keeps it alive for the tree's whole
 * lifetime. Views into that mapping are the design, not a hazard — the tree
 * may hold them or copy, and both are valid while the mapping lives.
 *
 * The hazard this guards is the class FastFHIR's test_no_dangling_views.py
 * guards: a view bound to a temporary, which dies before the walk runs. Here
 * the check is by execution rather than by spelling — the tree is built, the
 * construction stack fully unwinds, and every string the tree keeps is read
 * under ASan. A view into the live mapping reads fine; a view into a dead
 * temporary is a use-after-free and the test dies with it. The value checks
 * also prove the reads are intact, not merely non-crashing.
 *
 * Self-contained; non-zero exit on failure. ASan is the point — CI's
 * build-asan job builds the whole suite with -fsanitize=address,undefined.
 */
#include "IFE_Runtime.hpp"

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

using ::Iris::BYTE;

/// The snapshot bytes, heap-allocated: the mapping the tree is built from.
std::vector<BYTE> load_slide(const std::string& corpus_dir) {
    const std::string path = corpus_dir + "/v1_0_witness.test_slide";
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "FAIL: no snapshot at %s\n", path.c_str());
        ++g_failures;
        return {};
    }
    std::fseek(in, 0, SEEK_END);
    const auto size = static_cast<std::size_t>(std::ftell(in));
    std::fseek(in, 0, SEEK_SET);
    std::vector<BYTE> bytes(size);
    const auto read = std::fread(bytes.data(), 1, size, in);
    std::fclose(in);
    if (read != size) {
        std::fprintf(stderr, "FAIL: read %zu of %zu bytes\n", read, size);
        ++g_failures;
    }
    return bytes;
}

/// Build the abstraction over a live mapping and walk every string the tree
/// keeps. The mapping is the lens' owner — the caller keeps it alive, as the
/// VMA does in production — so views into it and copies are both valid while
/// it lives. The hazard this catches is a view bound to a temporary, which
/// dies before the walk runs (FastFHIR's test_no_dangling_views.py class).
void test_tree_reads_over_the_live_mapping(const std::string& corpus_dir) {
    const auto expected = v1_fixture::expectations();

    // The mapping stays alive for the whole walk, exactly as the arena does
    // in production; the tree reads over it as a lens.
    std::vector<BYTE> bytes = load_slide(corpus_dir);
    if (bytes.empty()) return;  // failure already counted
    const IrisCodec::Abstraction::File slide =
        IrisCodec::abstract_file_structure(bytes.data(), bytes.size());

    // ---- strings the tree keeps, read over the live mapping ------------- //
    IFE_CHECK(slide.header.fileSize != 0);

    // Image label: the map key and AssociatedImageInfo::imageLabel.
    IFE_CHECK(slide.images.size() == 1);
    const auto image = slide.images.find(expected.image_label);
    IFE_CHECK(image != slide.images.end());
    if (image != slide.images.end()) {
        IFE_CHECK(image->second.info.imageLabel == expected.image_label);
        IFE_CHECK(image->second.info.width == expected.image_width);
        IFE_CHECK(image->second.info.height == expected.image_height);
    }

    // Flat attribute map: key and value, copied or viewed, over the mapping.
    IFE_CHECK(slide.metadata.attributes.size() == 1);
    const auto attribute = slide.metadata.attributes.find(expected.attribute_key);
    IFE_CHECK(attribute != slide.metadata.attributes.end());
    if (attribute != slide.metadata.attributes.end()) {
        const std::string value(reinterpret_cast<const char*>(attribute->second.data()),
                                attribute->second.size());
        IFE_CHECK(value == expected.attribute_value);
    }

    // The attribute tree: keys, values and nested items, all over the mapping.
    IFE_CHECK(slide.attributeTree.size() == 1 + expected.nested_attributes.size());
    if (slide.attributeTree.size() == 1 + expected.nested_attributes.size()) {
        IFE_CHECK(slide.attributeTree[0].key == expected.attribute_key);
        IFE_CHECK(!slide.attributeTree[0].value.empty());
        for (std::size_t i = 0; i < expected.nested_attributes.size(); ++i) {
            const auto& sequence = expected.nested_attributes[i];
            const auto& node     = slide.attributeTree[i + 1];
            IFE_CHECK(node.key == sequence.key);
            for (std::size_t item = 0; item < node.items.size(); ++item) {
                for (std::size_t j = 0; j < node.items[item].size(); ++j) {
                    const auto& leaf = node.items[item][j];
                    const std::string value(reinterpret_cast<const char*>(leaf.value.data()),
                                            leaf.value.size());
                    IFE_CHECK(leaf.key == sequence.items[item][j].first);
                    IFE_CHECK(value == sequence.items[item][j].second);
                }
            }
        }
    }

    // ICC profile and annotation identifiers/titles, over the live mapping.
    IFE_CHECK(slide.metadata.ICC_profile == expected.icc_profile);
    for (const auto& [identifier, note] : slide.annotations) {
        (void)identifier;
        IFE_CHECK(note.offset != 0 || note.byteSize != 0);
    }
    for (const auto& [title, group] : slide.annotations.groups) {
        (void)group;
        IFE_CHECK(!title.empty());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: ife_lifetime_tests <corpus-dir>\n");
        return 2;
    }
    // Bazel cannot pass a directory; BUILD.bazel passes the runfiles path of
    // one corpus file and its parent is the directory CTest passes directly.
    test_tree_reads_over_the_live_mapping(ife_corpus_dir(argv[1]));

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ife_lifetime_tests: tree reads over the live mapping (ASan-clean)\n");
    return 0;
}
