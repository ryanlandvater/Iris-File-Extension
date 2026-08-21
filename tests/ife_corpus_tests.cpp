/**
 * @file ife_corpus_tests.cpp
 * @brief The corpus contains what the manifest says it contains.
 *
 * Four tests already read the hosted fixtures, and every one of them uses the
 * bytes as a fixture: they ask what a slide holds, not whether the corpus is
 * still the corpus. So a fixture that quietly stopped containing annotations
 * would keep all four green while the coverage it was hosted for silently
 * went away. That is not hypothetical -- ANNOTATION_JPEG was lost exactly
 * that way, and the loss was found later, by hand.
 *
 * This is the gate that makes the manifest's `blocks` array mean something.
 * Per slide fixture: validate it, walk the offset graph, and compare the
 * block types actually reached against the ones declared -- in BOTH
 * directions. A declared block the walk cannot find is a fixture that has
 * degraded. An undeclared block the walk does find is a manifest that has
 * fallen behind its own evidence; the manifest is the record, so it is an
 * error too, not a shrug.
 *
 * The walk itself is `generate_file_map`, which is the traversal: it follows
 * every offset from the file header and reports a typed entry per block.
 * Writing a second walk here would just be a second thing to be wrong.
 *
 * `recover_file_structure` runs too, and its types are unioned in. Two
 * reasons. The offset graph cannot see a tile frame at all -- a frame carries
 * no recovery tag and is addressed backward from the stream it precedes, so
 * only the recovery scan's signature match finds one, and without this a
 * framed fixture would be invisible to the gate. And it means the corpus
 * exercises the recovery path over real bytes, which nothing else does: on a
 * healthy file recovery should find a subset of the graph, and it does.
 *
 * Fragments (a bare block, not a slide) cannot be walked from a file header,
 * so they are size-checked and excluded from coverage accounting rather than
 * silently credited. Digests are verified at fetch time by
 * tools/fetch_corpus.py, which will not place a file that does not match.
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IrisFileExtension.hpp"

#include "corpus_manifest.hpp"
#include "ife_corpus_path.hpp"

#include <cstdio>
#include <set>
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
using ::IrisCodec::Abstraction::MapEntryType;

/// The manifest's name for a mapped block.
///
/// Deliberately a switch with no `default`: MapEntryType and the manifest
/// vocabulary have to stay in step, and a new entry type must not silently
/// map to "unknown" and pass. Adding one to IrisFileExtension.hpp and not here is
/// a compiler diagnostic, which is where that mistake should surface.
///
/// The two vocabularies differ in three places -- the map says TILE_DATA,
/// ASSOCIATED_IMAGES and ATTRIBUTES_BYTES where the specification says
/// TILE_PIXEL_DATA, IMAGES and ATTRIBUTE_BYTES. The manifest follows the
/// specification, because that is what a reader checking coverage against
/// the published block list will look for.
const char* manifest_name(MapEntryType type) {
    switch (type) {
        case ::IrisCodec::Abstraction::MAP_ENTRY_UNDEFINED:              return nullptr;
        case ::IrisCodec::Abstraction::MAP_ENTRY_FILE_HEADER:            return "FILE_HEADER";
        case ::IrisCodec::Abstraction::MAP_ENTRY_TILE_TABLE:             return "TILE_TABLE";
        case ::IrisCodec::Abstraction::MAP_ENTRY_CIPHER:                 return "CIPHER";
        case ::IrisCodec::Abstraction::MAP_ENTRY_METADATA:               return "METADATA";
        case ::IrisCodec::Abstraction::MAP_ENTRY_ATTRIBUTES:             return "ATTRIBUTES";
        case ::IrisCodec::Abstraction::MAP_ENTRY_LAYER_EXTENTS:          return "LAYER_EXTENTS";
        case ::IrisCodec::Abstraction::MAP_ENTRY_TILE_DATA:              return "TILE_PIXEL_DATA";
        case ::IrisCodec::Abstraction::MAP_ENTRY_TILE_OFFSETS:           return "TILE_OFFSETS";
        case ::IrisCodec::Abstraction::MAP_ENTRY_ATTRIBUTE_SIZES:        return "ATTRIBUTE_SIZES";
        case ::IrisCodec::Abstraction::MAP_ENTRY_ATTRIBUTES_BYTES:       return "ATTRIBUTE_BYTES";
        case ::IrisCodec::Abstraction::MAP_ENTRY_ASSOCIATED_IMAGES:      return "IMAGES";
        case ::IrisCodec::Abstraction::MAP_ENTRY_ASSOCIATED_IMAGE_BYTES: return "IMAGE_BYTES";
        case ::IrisCodec::Abstraction::MAP_ENTRY_ICC_PROFILE:            return "ICC_PROFILE";
        case ::IrisCodec::Abstraction::MAP_ENTRY_ANNOTATIONS:            return "ANNOTATIONS";
        case ::IrisCodec::Abstraction::MAP_ENTRY_ANNOTATION_BYTES:       return "ANNOTATION_BYTES";
        case ::IrisCodec::Abstraction::MAP_ENTRY_ANNOTATION_GROUP_SIZES: return "ANNOTATION_GROUP_SIZES";
        case ::IrisCodec::Abstraction::MAP_ENTRY_ANNOTATION_GROUP_BYTES: return "ANNOTATION_GROUP_BYTES";
        case ::IrisCodec::Abstraction::MAP_ENTRY_CLINICAL_METADATA:      return "CLINICAL_METADATA";
        case ::IrisCodec::Abstraction::MAP_ENTRY_TILE_FRAME:             return "TILE_FRAME";
    }
    return nullptr;
}

std::vector<BYTE> read_whole_file(const std::string& path) {
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "FAIL: cannot open %s\n", path.c_str());
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
        std::fprintf(stderr, "FAIL: short read on %s\n", path.c_str());
        ++g_failures;
        return {};
    }
    return bytes;
}

std::set<std::string> declared_blocks(const ife_corpus::Fixture& fixture) {
    std::set<std::string> out;
    for (const char* const* b = fixture.blocks; *b; ++b) out.insert(*b);
    return out;
}

/// Validate one slide fixture and return the block types its graph reaches.
std::set<std::string> walk(const ife_corpus::Fixture& fixture,
                           std::vector<BYTE>& bytes) {
    std::set<std::string> observed;

    const auto result =
        ::IrisCodec::validate_file_structure({bytes.data(), bytes.size()});
    if (result != ::Iris::IRIS_SUCCESS) {
        std::fprintf(stderr, "FAIL: %s: validation failed: %s\n",
                     fixture.name, result.message.c_str());
        ++g_failures;
        return observed;
    }

    // The offset graph, then the recovery scan; a block counts as reached if
    // either finds it. See the file comment for why both are needed.
    auto map = ::IrisCodec::generate_file_map({bytes.data(), bytes.size()});
    const auto recovered =
        ::IrisCodec::recover_file_structure({bytes.data(), bytes.size()});
    for (const auto& [offset, entry] : recovered) map.emplace(offset, entry);

    for (const auto& [offset, entry] : map) {
        const char* name = manifest_name(entry.type);
        if (!name) {
            // MAP_ENTRY_UNDEFINED in a map built from a file that just
            // validated means the map builder produced an entry it could not
            // classify -- corruption, or a block type the walk does not know.
            std::fprintf(stderr,
                         "FAIL: %s: unclassified map entry at offset %llu\n",
                         fixture.name,
                         static_cast<unsigned long long>(offset));
            ++g_failures;
            continue;
        }
        observed.insert(name);
    }
    return observed;
}

/// Check one fixture; returns the block types actually observed in it, empty
/// for anything that failed to load or was not walked.
std::set<std::string> test_fixture(const ife_corpus::Fixture& fixture,
                                   const std::string& corpus_dir) {
    const std::string path = std::string(corpus_dir) + "/" + fixture.name;
    std::vector<BYTE> bytes = read_whole_file(path);
    if (bytes.empty()) return {};

    // The manifest declares the size; fetch_corpus.py verifies the digest
    // before placing the file, so a size disagreement here means the cache
    // was touched after the fetch rather than that the host served junk.
    if (bytes.size() != fixture.size) {
        std::fprintf(stderr,
                     "FAIL: %s: %zu bytes on disk, manifest declares %llu\n",
                     fixture.name, bytes.size(), fixture.size);
        ++g_failures;
        return {};
    }

    if (fixture.kind == ife_corpus::FIXTURE_FRAGMENT) {
        // A bare block has no file header to walk from. Size-checked above,
        // digest-checked at fetch; its blocks are proven by whichever test
        // consumes it, and are deliberately NOT credited as coverage here.
        std::printf("  %-32s fragment, %llu B (not walked)\n",
                    fixture.name, fixture.size);
        return {};
    }

    const std::set<std::string> declared = declared_blocks(fixture);
    const std::set<std::string> observed = walk(fixture, bytes);
    if (observed.empty()) return {};  // walk() already reported why

    // Declared but not reached: the fixture has degraded, or was described
    // wrongly when it was hosted. Either way the corpus no longer proves what
    // the manifest sells it as proving.
    for (const auto& block : declared) {
        if (!observed.count(block)) {
            std::fprintf(stderr,
                         "FAIL: %s: manifest declares %s, the walk never "
                         "reached it\n", fixture.name, block.c_str());
            ++g_failures;
        }
    }

    // Reached but not declared: the manifest has fallen behind the bytes.
    // An error rather than a bonus, because the manifest is the record --
    // coverage that nothing records is coverage nobody can rely on.
    for (const auto& block : observed) {
        if (!declared.count(block)) {
            std::fprintf(stderr,
                         "FAIL: %s: the walk reached %s, which the manifest "
                         "does not declare — add it to \"blocks\"\n",
                         fixture.name, block.c_str());
            ++g_failures;
        }
    }

    std::printf("  %-32s slide, %llu B, %zu block types\n",
                fixture.name, fixture.size, observed.size());
    return observed;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: ife_corpus_tests <corpus-dir>\n");
        return 2;
    }
    const std::string corpus_dir = ife_corpus_dir(argv[1]);

    std::printf("ife_corpus_tests: %d fixtures\n", ife_corpus::fixture_count);

    std::set<std::string> reached;
    for (int i = 0; i < ife_corpus::fixture_count; ++i) {
        const auto& fixture = ife_corpus::fixtures[i];
        // What the walk actually found, never what the manifest declared.
        // Summing declared blocks credits coverage to a fixture that could
        // not even be opened -- which is exactly what this printed, "18 of
        // 18" beside two unopenable fixtures, before it was fixed.
        const auto observed = test_fixture(fixture, corpus_dir);
        reached.insert(observed.begin(), observed.end());
    }

    // The standing coverage number, printed rather than asserted: it is a
    // fact about how far the corpus has grown, and it grows by adding
    // fixtures, not by anything this test can do.
    //
    // TILE_FRAME is excluded from the count on purpose. It is the optional
    // 11-byte prefix on a tile stream, not one of the 18 blocks the
    // specification defines, so counting it would report 18/18 while a real
    // block was still unreached -- which is the false green this whole test
    // exists to prevent. It is reported on its own line instead.
    const bool frame = reached.erase("TILE_FRAME") > 0;
    std::printf("ife_corpus_tests: corpus reaches %zu of 18 block types\n",
                reached.size());
    std::printf("ife_corpus_tests: tile frame (TILE_FRAME) %s\n",
                frame ? "covered" : "NOT covered");

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ife_corpus_tests: every fixture holds what the manifest claims\n");
    return 0;
}
