/**
 * @file ife_recovery_bench_tests.cpp
 * @brief Bits flipped → percent recovered, as an assertion.
 *
 * The IFE counterpart of FastFHIR-benchmark's Test 5 (`bench/bench_test_5.*`,
 * the fig8 recovery curve), reduced to something ctest can run: one process,
 * one fixture, a seeded sweep, and a curve printed beside assertions that fail
 * the build. The curve is the benchmark; the assertions are why it lives here
 * rather than in a scratch directory where nobody runs it.
 *
 * FOUR THINGS THAT WENT WRONG IN THE FASTFHIR VERSION, AND ARE DESIGNED OUT
 * HERE. Each was measured there; none is hypothetical.
 *
 * 1. BOTH SIDES MUST COUNT THE SAME ATOM. Test 5's clean baseline counted
 *    `Bundle.entry` array elements while its damaged side counted recovered
 *    block references, and the ratio reported 1091% recovery on an UNDAMAGED
 *    file. Every published FFHR point was arithmetically meaningless. Here one
 *    function, `enumerate_units`, produces both sides — there is no second
 *    counter to drift.
 *
 * 2. A RATIO OF COUNTS CANNOT SEE MISATTACHMENT. If an edge comes back naming
 *    a different child, a count of recovered references is unchanged and the
 *    damage is invisible. So units are compared ANCHOR BY ANCHOR — keyed on
 *    the (parent, slot) that holds the edge — and a child that moved reads as
 *    a move instead of dissolving into one loss plus one invention. This is
 *    the property RC-9 exists to protect, and a benchmark that could not
 *    detect its absence could not show its value.
 *
 * 3. FINDABLE IS NOT INTACT. Every arm there once counted a unit as recovered
 *    on its header alone; obliterating 95.4% of one artifact's content still
 *    scored 100%. So content is hashed separately from identity.
 *
 * 4. THE DAMAGE MODEL IS PART OF THE CLAIM. Flipping bits uniformly across an
 *    IFE file would mostly hit tile pixel bytes, which this library does not
 *    interpret and does not protect — the curve would measure filler. Damage
 *    is therefore confined to STRUCTURAL bytes: everything except the payload
 *    runs of tile streams and byte arrays. Those runs are the documented
 *    "no second witness" boundary (IFE_Recovery.hpp), so including them would
 *    charge the format for bytes it never claimed to recover. The narrowness
 *    is the point and the limitation: this measures the two-witness
 *    reconciliation, not resilience to arbitrary injury.
 *
 * WHAT IS ASSERTED, AND WHY IT IS NOT EVERYTHING. Most outcomes in the table
 * below are damage this format never claimed to survive, and the first cut of
 * this test asserted them at zero and failed immediately — correctly. A flip
 * inside an inline scalar has no second witness; a flip in a NULL_OFFSET slot
 * turns absence into a pointer nothing can contradict; and a flip in an
 * UNFRAMED tile entry moves a stream that has no other witness at all.
 *
 * The one outcome with a real contract is MISATTACHMENT ON A WITNESSED EDGE:
 * an edge whose child carried an identity witness, coming back pointing at a
 * different real block. That is data reading as valid while being something
 * other than what was written — for a slide, another tile's pixels shown where
 * this one belongs — and it is what RC-9 closed. Measured here across both
 * fixtures: a FRAMED tile edge never misattaches, at any damage level, while
 * the unframed 1.0 fixture misattaches from four bits up. The two fixtures
 * priced the frame, and that is the number this file exists to keep honest.
 *
 * Block edges have NO identity witness today: a flipped offset landing on a
 * valid same-type block (METADATA's ATTRIBUTES slot moving between three
 * sibling ATTRIBUTES blocks, measured here) satisfies both witnesses at the
 * new address, so recovery classifies it Intact and is right to — nothing on
 * the wire contradicts it. Those are counted as `blind` and reported, never
 * asserted, until the format grows a witness that could catch them.
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IFE_Recovery.hpp"
#include "IFE_Primitives.hpp"

#include "ife_corpus_path.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
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
using Abstraction::BlockRef;
using Abstraction::FileMap;
using Abstraction::MapEntryType;
namespace p = Iris::File::primitives;
namespace k = Iris::File::constants;

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

// ── The unit ──────────────────────────────────────────────────────────────

/// The anchor: the slot that holds an edge. Two files are compared anchor by
/// anchor, so an edge whose child MOVED reads as a move rather than dissolving
/// into one loss plus one invention.
using Anchor = std::pair<Offset, Size>;

/// One parent→child reference, anchored. Identity is the first four fields;
/// `content` is deliberately NOT part of it, so the comparison can tell a unit
/// that VANISHED from one that is still there and no longer right.
struct Unit {
    Offset       parent;
    Size         slot;
    Offset       target;
    MapEntryType tag;
    std::uint64_t content;
    /// Did this edge's child carry an IDENTITY witness in the clean file — a
    /// tile frame naming which tile it is? Only these edges can be held to the
    /// no-misattachment contract; everything else has nothing on the wire that
    /// could contradict a pointer landing on a valid same-type block.
    bool witnessed = false;

    bool identity_lt(const Unit& o) const {
        if (parent != o.parent) return parent < o.parent;
        if (slot   != o.slot)   return slot   < o.slot;
        if (target != o.target) return target < o.target;
        return static_cast<int>(tag) < static_cast<int>(o.tag);
    }
    bool identity_eq(const Unit& o) const {
        return parent == o.parent && slot == o.slot &&
               target == o.target && tag == o.tag;
    }
};

/// FNV-1a. An equality check over bytes, never a security boundary — it only
/// has to be the same function on both sides of the comparison.
std::uint64_t content_hash(const BYTE* __p, std::size_t __n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < __n; ++i) { h ^= __p[i]; h *= 1099511628211ull; }
    return h;
}

/// THE ONE ENUMERATOR, used for the clean baseline and the recovered file
/// alike. Both sides therefore count the same atom by construction — the
/// defect that made every published FFHR curve meaningless was two functions
/// that were supposed to agree.
///
/// Each file is measured with its OWN map, so a damaged extent shows up as
/// changed content rather than being silently read from the clean file's idea
/// of the size.
std::map<Anchor, Unit> enumerate_units(const std::vector<BYTE>& __bytes) {
    const FileAccessInfo info{__bytes.data(), static_cast<Size>(__bytes.size())};
    const Recovery rec(info);
    const FileMap map = rec.scan();

    std::map<Anchor, Unit> units;
    for (const BlockRef& r : rec.reachable_blocks()) {
        if (r.target == k::NULL_OFFSET) continue;
        // The child's extent: its own claim where the census has one, and the
        // tile entry's SIZE for a stream, which is the only extent a tile has.
        Size extent = r.extent;
        if (const auto it = map.find(r.target); it != map.end() && it->second.size != 0)
            extent = it->second.size;
        std::uint64_t content = 0;
        if (extent != 0 && static_cast<std::uint64_t>(r.target) + extent <= __bytes.size())
            content = content_hash(__bytes.data() + r.target, extent);
        // A framed tile stream is the one child in the format that says WHICH
        // one it is. Every other edge relies on the parent's pointer alone to
        // establish identity, and a pointer is what damage moves.
        const bool witnessed =
            r.expected == Abstraction::MAP_ENTRY_TILE_PIXEL_DATA &&
            p::FrameHeader::signature_matches(__bytes.data(), r.target,
                                              static_cast<Size>(__bytes.size()));
        units.emplace(Anchor{r.parent, r.slot},
                      Unit{r.parent, r.slot, r.target, r.expected, content, witnessed});
    }
    return units;
}

/// Every offset the clean file has a block at — the reference set that tells a
/// misattachment from a dangling pointer. Sorted for binary search.
std::vector<Offset> real_block_offsets(const std::vector<BYTE>& __bytes) {
    const FileAccessInfo info{__bytes.data(), static_cast<Size>(__bytes.size())};
    const Recovery rec(info);
    std::vector<Offset> out;
    for (const auto& [off, e] : rec.scan())
        if (e.type != Abstraction::MAP_ENTRY_UNDEFINED) out.push_back(off);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ── The damage model ──────────────────────────────────────────────────────

bool is_byte_array(MapEntryType __t) {
    switch (__t) {
        case Abstraction::MAP_ENTRY_ATTRIBUTE_BYTES:
        case Abstraction::MAP_ENTRY_IMAGE_BYTES:
        case Abstraction::MAP_ENTRY_ICC_PROFILE:
        case Abstraction::MAP_ENTRY_ANNOTATION_BYTES:
        case Abstraction::MAP_ENTRY_ANNOTATION_GROUP_BYTES:
        case Abstraction::MAP_ENTRY_CLINICAL_METADATA:
            return true;
        default:
            return false;
    }
}

/// Every byte offset damage may target: the whole file MINUS the opaque
/// payload runs. A byte array keeps its header (the witnesses live there) and
/// loses its payload from the header onward; a tile stream is opaque end to
/// end — its frame sits BEHIND the anchor and stays targetable, which is
/// deliberate, because the frame is a witness this engine reads.
std::vector<Offset> structural_positions(const std::vector<BYTE>& __bytes) {
    const FileAccessInfo info{__bytes.data(), static_cast<Size>(__bytes.size())};
    const Recovery rec(info);
    const FileMap map = rec.scan();

    std::vector<bool> opaque(__bytes.size(), false);
    for (const auto& [off, e] : map) {
        if (e.size == 0) continue;
        std::uint64_t from = off;
        if (is_byte_array(e.type)) from = static_cast<std::uint64_t>(off)
                                        + p::ByteArrayHeader::HEADER_SIZE;
        else if (e.type != Abstraction::MAP_ENTRY_TILE_PIXEL_DATA) continue;
        for (std::uint64_t i = from;
             i < static_cast<std::uint64_t>(off) + e.size && i < __bytes.size(); ++i)
            opaque[static_cast<std::size_t>(i)] = true;
    }
    std::vector<Offset> out;
    for (std::size_t i = 0; i < __bytes.size(); ++i)
        if (!opaque[i]) out.push_back(static_cast<Offset>(i));
    return out;
}

// ── The comparison ────────────────────────────────────────────────────────

/// SIX OUTCOMES, because the first cut of this test had four and three of them
/// were measuring the wrong thing. Keyed on the ANCHOR — the (parent, slot)
/// that holds the edge — so an edge whose child MOVED is visible as a move
/// rather than dissolving into one loss plus one invention.
///
/// The split that matters is between what the two witnesses protect and what
/// nothing protects. IFE_Recovery.hpp names the second set exactly: inline
/// scalars, leaf payload bytes, an UNFRAMED tile stream (its entry is its own
/// only witness), and an absent slot — NULL_OFFSET is a value, so a flip in it
/// produces a pointer nothing can contradict. Damage there is undetectable by
/// construction, and counting it as a recovery failure would charge the format
/// for a promise it never made.
struct Outcome {
    std::size_t correct     = 0;  ///< same child, same bytes
    std::size_t altered     = 0;  ///< same child, bytes changed — unprotected
                                  ///< scalars and payload; no witness to fix it
    /// THE CONTRACT: the anchor names a DIFFERENT REAL BLOCK, and this edge's
    /// child carried an identity witness that should have prevented it.
    /// Reads as valid, is not what was written. RC-9 closed this; it must
    /// stay closed.
    std::size_t misattached = 0;
    /// The same move, on an edge with NO identity witness — an unframed tile
    /// stream, or any block edge (a flipped offset onto a valid same-type
    /// block satisfies both witnesses, so recovery calls it Intact and is
    /// right to). Undetectable damage, reported so the gap stays visible.
    std::size_t blind = 0;
    std::size_t dangling    = 0;  ///< child moved to somewhere that is not a
                                  ///< block — a damaged unprotected pointer; a
                                  ///< reader rejects it at the witness check
    std::size_t lost        = 0;  ///< the anchor is gone entirely
    std::size_t spurious    = 0;  ///< an anchor that did not exist clean: an
                                  ///< absent slot whose NULL took a flip
};

/// `__blocks` is the set of offsets the CLEAN file has a block at — the test for
/// whether a moved child landed on something that will read as valid. A move
/// onto a real block is misattachment; a move into open bytes is a dangling
/// pointer, which is damage but not deception.
Outcome compare(const std::map<Anchor, Unit>& __base, const std::map<Anchor, Unit>& __rec,
                const std::vector<Offset>& __blocks) {
    Outcome o;
    for (const auto& [anchor, b] : __base) {
        const auto it = __rec.find(anchor);
        if (it == __rec.end()) { ++o.lost; continue; }
        const Unit& r = it->second;
        if (r.target == b.target) {
            if (r.content == b.content) ++o.correct;
            else                        ++o.altered;
            continue;
        }
        if (!std::binary_search(__blocks.begin(), __blocks.end(), r.target)) { ++o.dangling; continue; }
        if (b.witnessed) ++o.misattached;
        else             ++o.blind;
    }
    for (const auto& [anchor, r] : __rec)
        if (!__base.count(anchor)) ++o.spurious;
    return o;
}

// ── The sweep ─────────────────────────────────────────────────────────────

void test_bit_to_percent_recovery(const char* __fixture) {
    auto clean = load_fixture(__fixture);
    if (clean.empty()) return;

    const std::map<Anchor, Unit> baseline = enumerate_units(clean);
    const std::vector<Offset> targets = structural_positions(clean);
    const std::vector<Offset> real = real_block_offsets(clean);
    IFE_CHECK(!baseline.empty());
    IFE_CHECK(!targets.empty());
    if (baseline.empty() || targets.empty()) return;

    // The floor P0-2 exists for: a harness that corrupts nothing recovers
    // everything, and reports a beautiful curve while doing so.
    IFE_CHECK(targets.size() * 8 > 64);

    std::printf("\n  %s — %zu anchored units, %zu structural bytes\n",
                __fixture, baseline.size(), targets.size());
    std::size_t witnessed_units = 0;
    for (const auto& [anchor, u] : baseline) if (u.witnessed) ++witnessed_units;
    std::printf("  %zu of them carry an identity witness (framed tiles)\n", witnessed_units);
    std::printf("  %5s %8s %8s %8s %9s %9s %7s %11s %s\n",
                "bits", "correct", "altered", "lost", "dangling", "spurious",
                "blind", "MISATTACH", "recovered");

    constexpr std::uint32_t TRIALS = 20;
    for (const std::uint32_t bits : {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
        Outcome total;
        // Seeded per level, so a failure is reproducible and a rerun of the
        // suite compares like with like.
        std::mt19937 rng(0x1F5u ^ (bits * 2654435761u));
        std::uniform_int_distribution<std::size_t> pick(0, targets.size() - 1);
        std::uniform_int_distribution<int> which_bit(0, 7);

        for (std::uint32_t t = 0; t < TRIALS; ++t) {
            std::vector<BYTE> g = clean;
            for (std::uint32_t b = 0; b < bits; ++b)
                g[targets[pick(rng)]] ^= static_cast<BYTE>(1u << which_bit(rng));

            // recover → apply → read back. Applying is the point: the question
            // is what a READER gets after recovery ran, not what the report
            // believed. apply() is all-or-nothing and rolls back on a failed
            // verify, so a refusal simply leaves the damaged bytes in place.
            const FileAccessInfo info{g.data(), static_cast<Size>(g.size())};
            Recovery rec(info);
            rec.apply(rec.recover());

            const Outcome o = compare(baseline, enumerate_units(g), real);
            total.correct     += o.correct;
            total.altered     += o.altered;
            total.misattached += o.misattached;
            total.blind       += o.blind;
            total.dangling    += o.dangling;
            total.lost        += o.lost;
            total.spurious    += o.spurious;
        }

        const double denom = static_cast<double>(baseline.size() * TRIALS);
        std::printf("  %5u %8zu %8zu %8zu %9zu %9zu %7zu %11zu %8.2f%%\n",
                    bits, total.correct, total.altered, total.lost, total.dangling,
                    total.spurious, total.blind, total.misattached,
                    100.0 * static_cast<double>(total.correct) / denom);

        // THE ONE HARD CONTRACT. An edge whose child carried an identity
        // witness must never come back naming a different real block: that is
        // data reading as valid while being something other than what was
        // written. RC-9 (TILE_INDEX adjudication + exclusivity) closed it, and
        // a number here is that work regressing, not a curve shifting.
        //
        // `blind` is deliberately NOT asserted: those edges have nothing on
        // the wire that could catch the move, so a zero there would be luck,
        // and asserting luck is how a suite starts lying.
        IFE_CHECK(total.misattached == 0);

        // The clean control: zero damage must reproduce the file exactly, so
        // an accounting change cannot hide inside a damaged run.
        if (bits == 0) {
            IFE_CHECK(total.correct == baseline.size() * TRIALS);
            IFE_CHECK(total.lost == 0 && total.altered == 0);
            IFE_CHECK(total.dangling == 0 && total.spurious == 0);
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

    test_bit_to_percent_recovery("v1_0_witness.test_slide");
    test_bit_to_percent_recovery("v1_1_witness.test_slide");

    if (g_failures) {
        std::fprintf(stderr, "ife_recovery_bench_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ife_recovery_bench_tests: recovery curve within contract\n");
    return 0;
}
