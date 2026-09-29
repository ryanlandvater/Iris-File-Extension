/**
 * @file ife_parser_tests.cpp
 * @brief The READ handle `Iris::File::Parser`: it owns its mapping when opened
 *        from a path, lifts the structure once, and hands out bounds-checked,
 *        zero-copy spans — the read side of the layered split (a codec's Slide
 *        holds a Parser and decodes; the Parser never decodes).
 *
 * What this pins, on the frozen 1.1 witness (one 2x2 layer declaring 3 focal
 * planes; tiles 0-2 framed with plane counts 3, 0, 0; tile 3 NULL_TILE):
 *   - `Parser::open` maps the file itself and keeps it mapped for as long as
 *     any copy of the handle lives.
 *   - `abstraction()` equals `abstract_file_structure` over the same bytes.
 *   - `tile()` spans point into the mapping at the entry's offset; NULL_TILE is
 *     an empty span, not an error; `tile_planes()` reads the frame.
 *   - `image()` returns the stream; bad indices and labels throw.
 *
 * Self-contained (no framework). Non-zero exit on failure. argv[1] is the
 * corpus directory (CTest) or a corpus file's runfiles path (Bazel).
 */
#include "IFE_Parser.hpp"
#include "IrisFileExtension.hpp"
#include "ife_corpus_path.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Iris;
using namespace Iris::File;

namespace k = ::Iris::File::constants;

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

template <class E>
bool throws(const std::function<void()>& fn) {
    try { fn(); } catch (const E&) { return true; } catch (...) { return false; }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ife_parser_tests <corpus dir>\n");
        return 2;
    }
    const std::string witness = ife_corpus_dir(argv[1]) + "/v1_1_witness.test_slide";

    // ---- open: the parser owns its mapping --------------------------------- //
    Parser parser = Parser::open(witness);
    IFE_CHECK(static_cast<bool>(parser));
    IFE_CHECK(parser->owns_mapping());
    IFE_CHECK(parser.validate_file_structure() == IRIS_SUCCESS);

    // ---- abstraction() is abstract_file_structure over the same bytes ------ //
    const std::vector<BYTE> bytes = ife_read_file(witness);
    IFE_CHECK(bytes.size() == parser.size());
    const Abstraction::File borrowed = abstract_file_structure({bytes.data(), bytes.size()});
    const Abstraction::File& lifted  = parser.abstraction();
    IFE_CHECK(&lifted == &parser.abstraction());   // lifted once, kept
    IFE_CHECK(lifted.header.fileSize == borrowed.header.fileSize);
    IFE_CHECK(lifted.tileTable.extent.layers.size() == borrowed.tileTable.extent.layers.size());
    IFE_CHECK(lifted.tileTable.planes == borrowed.tileTable.planes);
    IFE_CHECK(lifted.tileTable.layers.size() == borrowed.tileTable.layers.size());
    for (size_t l = 0; l < lifted.tileTable.layers.size() && l < borrowed.tileTable.layers.size(); ++l) {
        IFE_CHECK(lifted.tileTable.layers[l].size() == borrowed.tileTable.layers[l].size());
        for (size_t t = 0; t < lifted.tileTable.layers[l].size() &&
                           t < borrowed.tileTable.layers[l].size(); ++t) {
            IFE_CHECK(lifted.tileTable.layers[l][t].offset == borrowed.tileTable.layers[l][t].offset);
            IFE_CHECK(lifted.tileTable.layers[l][t].size   == borrowed.tileTable.layers[l][t].size);
        }
    }
    IFE_CHECK(lifted.images.size() == borrowed.images.size());
    IFE_CHECK(lifted.metadata.attributes == borrowed.metadata.attributes);

    // ---- tile(): zero-copy spans; NULL_TILE is empty ----------------------- //
    IFE_CHECK(lifted.tileTable.layers.size() == 1);
    if (lifted.tileTable.layers.size() == 1 && lifted.tileTable.layers[0].size() == 4) {
        for (uint32_t t = 0; t < 4; ++t) {
            const auto& entry = lifted.tileTable.layers[0][t];
            const auto span = parser.tile(0, t);
            if (entry.offset == k::NULL_TILE) {
                IFE_CHECK(t == 3);
                IFE_CHECK(span.empty());
            } else {
                IFE_CHECK(span.data() == parser.data() + entry.offset);   // no copy
                IFE_CHECK(span.size() == entry.size);
                IFE_CHECK(!span.empty() && span[0] == 0xCD);
            }
        }
        // tile_planes(): the frame's count (0 means one), NULL_TILE is zero.
        IFE_CHECK(parser.tile_planes(0, 0) == 3);
        IFE_CHECK(parser.tile_planes(0, 1) == 1);
        IFE_CHECK(parser.tile_planes(0, 2) == 1);
        IFE_CHECK(parser.tile_planes(0, 3) == 0);
    } else {
        IFE_CHECK(!"the 1.1 witness is one 2x2 layer");
    }
    IFE_CHECK(throws<std::out_of_range>([&] { (void)parser.tile(1, 0); }));
    IFE_CHECK(throws<std::out_of_range>([&] { (void)parser.tile(0, 4); }));
    IFE_CHECK(throws<std::out_of_range>([&] { (void)parser.tile_planes(0, 4); }));

    // ---- image(): the stream, zero-copy ------------------------------------ //
    const auto thumbnail = lifted.images.find("thumbnail");
    IFE_CHECK(thumbnail != lifted.images.end());
    if (thumbnail != lifted.images.end()) {
        const auto span = parser.image("thumbnail");
        IFE_CHECK(span.data() == parser.data() + thumbnail->second.offset);
        IFE_CHECK(span.size() == thumbnail->second.byteSize);
        IFE_CHECK(span.size() == 48 && span[0] == 0xAB);   // the writer's stream
    }
    IFE_CHECK(throws<std::out_of_range>([&] { (void)parser.image("no such label"); }));

    // ---- lifetime: the mapping lives as long as any copy of the handle ---- //
    {
        Parser copy = parser;
        const BYTE* before = copy.data();
        parser.reset();                      // drop the original handle
        IFE_CHECK(!parser);
        IFE_CHECK(copy.data() == before);
        IFE_CHECK(copy.validate_file_structure() == IRIS_SUCCESS);   // still mapped
        IFE_CHECK(copy.tile(0, 0).size() > 0 && copy.tile(0, 0)[0] == 0xCD);
    }

    // ---- a borrowed parser does not own ------------------------------------ //
    {
        const Parser borrowed_parser({bytes.data(), bytes.size()});
        IFE_CHECK(!borrowed_parser->owns_mapping());
        IFE_CHECK(borrowed_parser.tile_planes(0, 0) == 3);
    }

    // ---- a path that cannot be mapped throws ------------------------------- //
    IFE_CHECK(throws<std::runtime_error>([&] {
        (void)Parser::open(ife_corpus_dir(argv[1]) + "/no_such_slide.iris"); }));

    if (g_failures == 0) std::printf("ife_parser_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
