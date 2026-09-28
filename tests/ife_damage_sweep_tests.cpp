/**
 * @file ife_damage_sweep_tests.cpp
 * @brief Exhaustive single-site damage sweep over the corpus witnesses.
 *
 * WHY THIS EXISTS AS ITS OWN TARGET. The RC-3 suite damages the sites a
 * human chose; this one damages EVERY site. On 2026-08-28 that difference was
 * a heap-buffer-overflow: `ife_recovery_tests` passed clean under
 * -fsanitize=address,undefined while the engine read eight bytes from before
 * the mapping, because no hand-written test flipped a bit in an unused
 * NULL_OFFSET slot — and a sanitizer only reports what the tests execute. The
 * sweep's real value is therefore under a sanitizer, where it is the gate
 * that catches the next one; but every invariant below is checkable without
 * one, and two of them (a refused apply() changing nothing, entries and gaps
 * inside the file) are the ones a bounds defect breaks first.
 *
 * For each fixture, each byte offset, each of three masks — a whole byte, the
 * low bit, the high bit — the damaged copy is run through the full engine:
 * scan -> recover -> apply -> re-scan -> re-recover. The invariants:
 *
 *   1. every map entry lies inside the file
 *   2. every gap lies inside the file
 *   3. apply() returning false leaves the bytes EXACTLY as they were
 *      (the all-or-nothing contract; it wrote as it went until 2026-08-28)
 *   4. a repaired file re-scans and re-recovers without a crash, and
 *      re-applying its report is idempotent-safe
 *   5. the sweep actually damages things (P0-2's floor: a harness that
 *      corrupts nothing recovers everything)
 *   6. the READ PATH never runs off the file (RC-10.1) -- on the damaged copy
 *      AND on the repaired one, which is the file a consumer opens next.
 *      abstract_file_structure() once copied a wire COUNT straight into
 *      std::string::assign: one flipped bit read a gigabyte past an 832-byte
 *      file while every witness agreed and recovery correctly reported
 *      nothing wrong. It may throw; it may never read out of bounds, and only
 *      a sanitizer sees the difference. This suite recovered, and never read.
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IFE_Recovery.hpp"

#include "ife_corpus_path.hpp"

#include <cstdio>
#include <cstdlib>
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

using Iris::BYTE;
using namespace Iris::File;
using Abstraction::FileMap;
using Abstraction::Gap;
using Abstraction::RecoveryReport;
using Abstraction::RepairClass;

std::string g_corpus_dir;

std::vector<BYTE> load_fixture(const char* __name) {
    const std::string path = g_corpus_dir + "/" + __name;
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "FAIL: no fixture at %s\n", path.c_str());
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
        std::fprintf(stderr, "FAIL: short read from %s\n", path.c_str());
        ++g_failures;
        return {};
    }
    return bytes;
}

/// Invariants 1 and 2: nothing the engine reports may name bytes the file
/// does not have. A wrapped bounds check shows up here first.
void check_inside_the_file(const FileMap& __map) {
    for (const auto& [off, e] : __map)
        IFE_CHECK(static_cast<std::uint64_t>(off) + e.size
                  <= static_cast<std::uint64_t>(__map.file_size));
    for (const Gap& g : __map.gaps)
        IFE_CHECK(static_cast<std::uint64_t>(g.start) + g.length
                  <= static_cast<std::uint64_t>(__map.file_size));
}

struct Tally {
    std::size_t iterations   = 0;
    std::size_t damaged      = 0;   ///< verdicts that were not Intact
    std::size_t applied      = 0;
    std::size_t refused      = 0;
    std::size_t holes        = 0;
    std::size_t hole_repairs = 0;
    std::size_t ambiguous    = 0;
    std::size_t read_ok      = 0;   ///< abstract_file_structure returned
    std::size_t read_refused = 0;   ///< ...or threw, which is allowed
};

/// Invariant 6: every public read entry point, on bytes that may be damaged.
/// Throwing is a legitimate answer; reading outside `__f` is not, and is what
/// running this suite under ASan exists to catch. Returns whether the full
/// abstraction was produced.
bool read_path(const std::vector<BYTE>& __f) {
    const FileAccessInfo info{__f.data(), static_cast<Size>(__f.size())};
    (void)validate_file_structure(info);
    // Called, not checked against invariant 1: generate_file_map() records
    // what the offset graph CLAIMS over a damaged file, so a corrupt COUNT
    // legitimately yields an entry past EOF. What it may not do is read one.
    try { (void)generate_file_map(info); }
    catch (const std::exception&) {}
    try { (void)abstract_file_structure(info); return true; }
    catch (const std::exception&) { return false; }
}

void sweep(const char* __name, Tally& __t) {
    const std::vector<BYTE> clean = load_fixture(__name);
    if (clean.empty()) return;
    // The control: a strict read path that refuses an UNDAMAGED file is
    // broken, however safe it is.
    IFE_CHECK(read_path(clean));
    constexpr BYTE masks[] = {0xFF, 0x01, 0x80};

    for (std::size_t at = 0; at < clean.size(); ++at) {
        for (const BYTE mask : masks) {
            std::vector<BYTE> f = clean;
            f[at] ^= mask;
            ++__t.iterations;

            Recovery rec({f.data(), f.size()});
            const FileMap map = rec.scan();
            check_inside_the_file(map);
            if (read_path(f)) ++__t.read_ok; else ++__t.read_refused;

            const RecoveryReport rep = rec.recover();
            for (const auto& v : rep.blocks)
                if (v.class_ != RepairClass::Intact) ++__t.damaged;
            __t.holes        += rep.holes;
            __t.hole_repairs += rep.hole_corroborated;
            __t.ambiguous    += rep.ambiguous;

            // Invariant 3: the all-or-nothing contract.
            const std::vector<BYTE> before = f;
            if (rec.apply(rep)) {
                ++__t.applied;
            } else {
                ++__t.refused;
                IFE_CHECK(f == before);
            }

            // Invariant 4: whatever apply() did, the result is still a file
            // the engine can read without running off it.
            Recovery after({f.data(), f.size()});
            const FileMap remap = after.scan();
            check_inside_the_file(remap);
            (void)read_path(f);
            const RecoveryReport reagain = after.recover();
            const std::vector<BYTE> settled = f;
            if (!after.apply(reagain)) IFE_CHECK(f == settled);

            if (g_failures > 20) {   // a systemic break; stop shouting
                std::fprintf(stderr, "aborting sweep of %s at offset %zu\n", __name, at);
                return;
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 2;
    }
    g_corpus_dir = ife_corpus_dir(argv[1]);

    Tally t;
    sweep("cipher_iris.test_slide", t);
    sweep("v1_0_witness.test_slide", t);
    sweep("v1_1_witness.test_slide", t);

    std::printf("    swept %zu single-site damages: %zu damaged verdicts, "
                "%zu holes, %zu hole repairs, %zu ambiguous, %zu applied / %zu refused\n",
                t.iterations, t.damaged, t.holes, t.hole_repairs, t.ambiguous,
                t.applied, t.refused);
    std::printf("    read path over the damaged copies: %zu read, %zu refused\n",
                t.read_ok, t.read_refused);

    // Invariant 5 (P0-2): the sweep must actually break things. A fixture
    // that stopped loading, or damage that stopped taking, would otherwise
    // report a clean run.
    IFE_CHECK(t.iterations > 10000);
    IFE_CHECK(t.damaged > 0);
    IFE_CHECK(t.holes > 0);

    if (g_failures) {
        std::fprintf(stderr, "ife_damage_sweep_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ife_damage_sweep_tests: all invariants held\n");
    return 0;
}
