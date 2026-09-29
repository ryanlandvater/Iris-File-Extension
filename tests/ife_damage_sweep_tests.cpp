/**
 * @file ife_damage_sweep_tests.cpp
 * @brief Every public read entry point, over every single bit flip of every
 *        fixture.
 *
 * WHY THIS EXISTS AS ITS OWN TARGET. Hand-written tests damage the sites a
 * human chose; this damages EVERY site, and a sanitizer only reports what the
 * tests execute. On 2026-08-28 that difference was a heap-buffer-overflow no
 * hand-written test reached. The read path is where it matters most: once,
 * abstract_file_structure() copied a wire COUNT straight into
 * std::string::assign, and one flipped bit read a gigabyte past an 832-byte
 * file. So the sweep's real value is under ASan/UBSan (CI's sanitizer leg,
 * `ctest --preset asan`), where it is the gate that catches the next one.
 *
 * Restored 2026-09-28 (MIGRATION.md RB-1) without the recovery engine it used
 * to drive; its repaired-copy half returns with the rebuilt engine.
 *
 * Single flips, one at a time, over every bit of every fixture — the shape of
 * FastFHIR's census_single_flip_sweep, and the threat model recovery assumes.
 * Each flipped file goes through is_iris_codec_file, validate_file_structure,
 * generate_file_map, abstract_file_structure and a Parser bound to it, then
 * the bit is flipped back. Throwing is a legitimate answer. The invariants:
 *
 *   1. an undamaged fixture reads completely (the control: a read path that
 *      refuses a good file is broken, however safe it is);
 *   2. nothing reads outside the file — ASan's half of the contract;
 *   3. a file that validates can be read — validation is the gate a caller
 *      checks before reading, so it may not pass what the abstraction refuses;
 *   4. an abstraction that is returned hands out only ranges inside the file:
 *      every tile entry that is not NULL_TILE, and every image;
 *   5. the Parser's spans lie inside the mapping, and tile_planes() answers
 *      for every tile the abstraction accepted;
 *   6. the sweep actually damages things (a harness that corrupts nothing
 *      passes everything).
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IFE_Builder.hpp"
#include "IFE_Parser.hpp"

#include "ife_builder_fixture.hpp"
#include "ife_corpus_path.hpp"

#include <cstdio>
#include <exception>
#include <span>
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

using namespace Iris;
using namespace Iris::File;
namespace k = ::Iris::File::constants;

/// [offset, offset + size) inside a file of `file_size` bytes, without the
/// sum that wraps (CLAUDE.md, "Never add to an offset to bound-check it").
bool inside(Offset offset, Size size, Size file_size) {
    return offset <= file_size && file_size - offset >= size;
}

bool inside(std::span<const BYTE> span, const std::vector<BYTE>& file) {
    return span.empty() ||
           (span.data() >= file.data() &&
            inside(static_cast<Offset>(span.data() - file.data()), span.size(), file.size()));
}


struct Tally {
    std::size_t iterations = 0;
    std::size_t validated  = 0;   ///< validate_file_structure passed
    std::size_t abstracted = 0;   ///< abstract_file_structure returned
};

/// Invariant 4: what a returned abstraction hands out can be read through.
void check_abstraction(const Abstraction::File& file, Size file_size) {
    for (const auto& layer : file.tileTable.layers)
        for (const auto& entry : layer)
            IFE_CHECK(entry.offset == k::NULL_TILE || inside(entry.offset, entry.size, file_size));
    for (const auto& [label, image] : file.images)
        IFE_CHECK(inside(image.offset, image.byteSize, file_size));
}

/// Invariant 5: the Parser's handles over the same bytes.
void check_parser(const std::vector<BYTE>& f) {
    const Parser parser(FileAccessInfo{f.data(), f.size()});
    const Abstraction::File* file = nullptr;
    try { file = &parser.abstraction(); }
    catch (const std::exception&) { return; }   // refused: already counted by the caller

    // The abstraction accepted every tile and image below, so each span must
    // be producible — a throw here is the Parser disagreeing with it.
    for (std::uint32_t l = 0; l < file->tileTable.layers.size(); ++l)
        for (std::uint32_t t = 0; t < file->tileTable.layers[l].size(); ++t) {
            try {
                IFE_CHECK(inside(parser.tile(l, t), f));
                (void)parser.tile_planes(l, t);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "FAIL: layer %u, tile %u was accepted, then refused: %s\n",
                             l, t, e.what());
                ++g_failures;
            }
        }
    for (const auto& [label, image] : file->images) {
        try {
            IFE_CHECK(inside(parser.image(label), f));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: image '%s' was accepted, then refused: %s\n",
                         label.c_str(), e.what());
            ++g_failures;
        }
    }
}

/// Every public read entry point over `f`. Returns whether it read completely.
bool read_path(const std::vector<BYTE>& f, Tally& tally) {
    const FileAccessInfo info{f.data(), f.size()};
    (void)is_iris_codec_file(info);
    const bool valid = validate_file_structure(info) == IRIS_SUCCESS;
    // generate_file_map() records what the offset graph CLAIMS, so over a
    // damaged file an entry past EOF is legitimate. Reading one is not.
    try { (void)generate_file_map(info); }
    catch (const std::exception&) {}

    bool read = false;
    try {
        check_abstraction(abstract_file_structure(info), f.size());
        read = true;
    } catch (const std::exception&) {}
    check_parser(f);
    IFE_CHECK(read || !valid);   // invariant 3

    tally.validated  += valid;
    tally.abstracted += read;
    return valid && read;
}

void sweep(const char* name, const std::vector<BYTE>& clean, Tally& tally) {
    if (clean.empty()) {
        std::fprintf(stderr, "FAIL: fixture %s is empty\n", name);
        ++g_failures;
        return;
    }
    Tally control;
    if (!read_path(clean, control)) {   // invariant 1
        std::fprintf(stderr, "FAIL: the undamaged %s does not read\n", name);
        ++g_failures;
    }

    // Flipped and restored in place: the buffer stays exactly the file's size,
    // so a read one byte past it is still a heap overflow ASan reports.
    std::vector<BYTE> f = clean;
    for (std::size_t at = 0; at < f.size(); ++at)
        for (int bit = 0; bit < 8; ++bit) {
            f[at] ^= static_cast<BYTE>(1u << bit);
            ++tally.iterations;
            (void)read_path(f, tally);
            f[at] ^= static_cast<BYTE>(1u << bit);
            if (g_failures > 20) {   // a systemic break; stop shouting
                std::fprintf(stderr, "aborting the sweep of %s at byte %zu, bit %d\n",
                             name, at, bit);
                return;
            }
        }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 2;
    }
    const std::string corpus = ife_corpus_dir(argv[1]);

    Tally t;
    for (const char* name : {"cipher_iris.test_slide", "v1_0_witness.test_slide",
                             "v1_1_witness.test_slide"})
        sweep(name, ife_load_fixture(corpus, name), t);
    sweep("the Builder fixture", ife_builder_fixture(), t);

    std::printf("    swept %zu single flips: %zu validated, %zu abstracted\n",
                t.iterations, t.validated, t.abstracted);

    // Invariant 6: the sweep must break things, and must not break everything.
    IFE_CHECK(t.iterations > 10000);
    IFE_CHECK(t.abstracted < t.iterations);
    IFE_CHECK(t.abstracted > 0);

    if (g_failures) {
        std::fprintf(stderr, "ife_damage_sweep_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ife_damage_sweep_tests: all invariants held\n");
    return 0;
}
