/**
 * @file ife_recovery_tests.cpp
 * @brief RC-3.1…RC-3.5 — the two-witness reconciliation against damaged bytes.
 *
 * Ported from FastFHIR's test_recovery.cpp (REC-18's tests) onto IFE's
 * writer-produced corpus witnesses: every fixture below is the bytes a real
 * encoder wrote, fetched by digest (tests/corpus/manifest.json), never a
 * hand-built buffer — the COV-1 reason. Damage is applied to the in-memory
 * copy.
 *
 * The engine under test is the Recovery class (IFE_Recovery.cpp): scan()
 * (the census + gap sweep), recover() (the join + classification), apply()
 * (the only mutating path). The rules these tests pin:
 *
 *   RC-3.1  a clean stream reports zero damage — every edge Intact, no holes,
 *           no gaps, no fabricated references (D3/D4/D5).
 *   RC-3.2  one damaged witness per repair class, reported with its class and
 *           Hamming bit cost, never silently (P0-2 floor: the damage took).
 *   RC-3.3  both witnesses destroyed: the block vanishes from scan(), and the
 *           unattributed run is reported as a Hole at its exact offset and
 *           extent — per shape, because the extent rules differ per shape.
 *   RC-3.4  the IFE-only case: a surviving tile frame corroborates a broken
 *           tile-offsets entry; apply() restores it. With the frame also
 *           destroyed, the run is a Hole, never a guess.
 *   RC-3.5  never silent: Ambiguous ties reported with their candidates and
 *           never picked; out-of-budget damage reported Unrecovered; apply()
 *           on a zero-repair report is byte-identical.
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IFE_Recovery.hpp"
#include "IFE_Blocks.hpp"        // generated offsets for the damage sites
#include "IFE_Primitives.hpp"   // the universal header's RECOVERY slot

#include "ife_corpus_path.hpp"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

using Iris::BYTE;
using namespace Iris::File;
namespace b  = ::Iris::File::blocks;
namespace k  = ::Iris::File::constants;
namespace p  = ::Iris::File::primitives;
using Abstraction::BlockRef;
using Abstraction::BlockVerdict;
using Abstraction::FileMap;
using Abstraction::Gap;
using Abstraction::GapClass;
using Abstraction::MapEntryType;
using Abstraction::ProducerFailure;
using Abstraction::ProducerFailureKind;
using Abstraction::RecoveryReport;
using Abstraction::RepairClass;

std::string g_corpus_dir;

/// Load one pinned corpus witness into a mutable copy. Writer-produced bytes
/// (the manifest pins them by digest); damage below is applied to the copy.
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

/// The verdict for one reference, matched on (parent, slot) — the two values
/// that identify the edge, exactly as apply() addresses it.
const BlockVerdict* find_verdict(const RecoveryReport& __rep, Offset __parent,
                                 Size __slot) {
    for (const auto& v : __rep.blocks)
        if (v.block.parent == __parent && v.block.slot == __slot) return &v;
    return nullptr;
}

/// First reference whose child is expected to be `__type`. Null when the
/// fixture does not encode that edge — the caller must not assume shapes.
const BlockVerdict* find_ref(const RecoveryReport& __rep, MapEntryType __type) {
    for (const auto& v : __rep.blocks)
        if (v.block.expected == __type) return &v;
    return nullptr;
}

/// One byte must really have changed, or every assertion below it passes for
/// the wrong reason (P0-2's floor: a harness that corrupts nothing recovers
/// everything). `__one_bit` flips a single bit (cost-1 classes) instead of
/// the whole byte (both-witness orphaning).
void check_flip_took(std::vector<BYTE>& __f, Offset __at, bool __one_bit) {
    const BYTE before = __f[__at];
    __f[__at] ^= __one_bit ? 0x01 : 0xFF;
    IFE_CHECK(__f[__at] != before);
}

// ── RC-3.1 — clean-stream zero-damage regression ──────────────────────────

void test_clean_stream_zero_damage() {
    // cipher_iris covers every block (all 18 + TILE_FRAME); v1_1 covers the
    // 1.1 set with frames, a NULL_TILE slot and varying Z_PLANES. Both must
    // report zero damage: this is the D3/D4/D5 regression — legal NULL slots,
    // packed values and empty sequences must not invent edges or extents.
    for (const char* name : {"cipher_iris.test_slide", "v1_1_witness.test_slide"}) {
        auto f = load_fixture(name);
        if (f.empty()) continue;
        Recovery rec({f.data(), f.size()});
        const RecoveryReport rep = rec.recover();

        // P0-2 floor: the comparison is meaningless on an empty set.
        IFE_CHECK(rep.blocks_total > 0);
        IFE_CHECK(rep.intact == rep.blocks_total);
        IFE_CHECK(rep.corroborated == 0 && rep.tag_repaired == 0 &&
                  rep.position_repaired == 0 && rep.extent_derived == 0 &&
                  rep.ambiguous == 0 && rep.unrecovered == 0);
        IFE_CHECK(rep.holes == 0 && rep.version_skew == 0 && rep.gaps.empty());

        // The RC-2.1 tiling probe: every entry has a derived extent and the
        // arena tiles exactly — Σ extent == file_size, 0 gaps, 0 overlaps.
        const FileMap map = rec.scan();
        std::uint64_t sum = 0;
        std::size_t unsized = 0;
        for (const auto& [off, e] : map) {
            sum += e.size;
            if (e.size == 0) ++unsized;
        }
        IFE_CHECK(unsized == 0);
        IFE_CHECK(sum == f.size());
        IFE_CHECK(map.gaps.empty());
    }

    // The frozen 1.0 witness carries a known writer defect — its METADATA
    // CLINICAL_OFFSET names the ATTRIBUTES block (a stale slot; no
    // CLINICAL_METADATA block exists). The engine must report exactly that
    // inconsistency and NOTHING else. The verdict is Unrecovered, not
    // TagRepaired (RC-8 / REC-22.2): the block at the slot READS as its own
    // type — its slots corroborate under ATTRIBUTES, not under
    // CLINICAL_METADATA — so it is an innocent block the writer's stale slot
    // landed on, and relabelling it (the old TagRepaired) would destroy real
    // data. The audit records the mismatch; nothing is guessed.
    {
        auto f = load_fixture("v1_0_witness.test_slide");
        if (f.empty()) return;
        Recovery rec({f.data(), f.size()});
        const RecoveryReport rep = rec.recover();
        IFE_CHECK(rep.tag_repaired == 0);
        IFE_CHECK(rep.corroborated == 0 && rep.position_repaired == 0 &&
                  rep.extent_derived == 0 && rep.ambiguous == 0);
        IFE_CHECK(rep.holes == 0 && rep.gaps.empty());
        IFE_CHECK(rep.intact + rep.unrecovered == rep.blocks_total);
        // the one inconsistency is the CLINICAL edge
        const BlockVerdict* v = find_ref(rep, MapEntryType::MAP_ENTRY_CLINICAL_METADATA);
        IFE_CHECK(v != nullptr && v->class_ == RepairClass::Unrecovered);
        bool audited = false;
        for (const ProducerFailure& fl : rep.failures)
            if (fl.kind == ProducerFailureKind::VTableRecoveryMismatch)
                audited = true;
        IFE_CHECK(audited);   // reported, never silent — and never relabelled
    }
}

// ── RC-3.2 — one witness per class, with its bit cost ─────────────────────

void test_one_witness_per_class() {
    auto f = load_fixture("v1_1_witness.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport base = clean.recover();
    IFE_CHECK(base.blocks_total > 0);

    // PositionRepaired @1 — child VALIDATION broken, parent names it.
    {
        const BlockVerdict* r = find_ref(base, MapEntryType::MAP_ENTRY_IMAGE_BYTES);
        IFE_CHECK(r != nullptr && r->class_ == RepairClass::Intact);
        if (!r || r->class_ != RepairClass::Intact) return;
        auto g = f;
        check_flip_took(g, r->block.target, true);          // VALIDATION u64, 1 bit
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
        IFE_CHECK(v != nullptr && v->class_ == RepairClass::PositionRepaired);
        IFE_CHECK(v != nullptr && v->bit_cost == 1);
    }

    // Corroborated @1 — parent slot broken (MSB: the corrupted target lands
    // far out of bounds, so no other block can tie it), the child survives
    // self-consistent but unreachable -> a unique orphan names it.
    {
        const BlockVerdict* r = find_ref(base, MapEntryType::MAP_ENTRY_IMAGES);
        IFE_CHECK(r != nullptr && r->class_ == RepairClass::Intact);
        if (!r || r->class_ != RepairClass::Intact) return;
        auto g = f;
        check_flip_took(g, r->block.parent + r->block.slot + 7, true);  // u64 MSB byte, 1 bit
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
        IFE_CHECK(v != nullptr && v->class_ == RepairClass::Corroborated);
        IFE_CHECK(v != nullptr && v->bit_cost == 1);
        IFE_CHECK(v != nullptr && v->repaired == r->block.target);
    }

    // TagRepaired @1 — child RECOVERY broken; the slot's compiled expectation
    // rewrites it.
    {
        const BlockVerdict* r = find_ref(base, MapEntryType::MAP_ENTRY_IMAGE_BYTES);
        IFE_CHECK(r != nullptr && r->class_ == RepairClass::Intact);
        if (!r || r->class_ != RepairClass::Intact) return;
        auto g = f;
        check_flip_took(g, r->block.target + b::IMAGE_BYTES::offset::RECOVERY, true);  // u16 tag, 1 bit
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
        IFE_CHECK(v != nullptr && v->class_ == RepairClass::TagRepaired);
        IFE_CHECK(v != nullptr && v->bit_cost == 1);
    }

    // ExtentDerived — array COUNT broken (a high byte: the claimed extent
    // grows past the next entry, so the tiling recomputes it).
    {
        const BlockVerdict* r = find_ref(base, MapEntryType::MAP_ENTRY_LAYER_EXTENTS);
        IFE_CHECK(r != nullptr && r->class_ == RepairClass::Intact);
        if (!r || r->class_ != RepairClass::Intact) return;
        auto g = f;
        const Offset count_at = r->block.target + b::LAYER_EXTENTS::offset::COUNT;
        check_flip_took(g, count_at + 3, false);      // a whole byte of the u32 COUNT
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
        IFE_CHECK(v != nullptr && v->class_ == RepairClass::ExtentDerived);
        IFE_CHECK(v != nullptr && v->bit_cost > 0);
        IFE_CHECK(v != nullptr && v->repaired > 0);   // the recomputed extent
        // Exact arithmetic: the recomputed extent is the distance to the next
        // claimed entry (the tiling's limit), from the damaged map.
        const FileMap m_dmg = rec.scan();
        const auto nxt = m_dmg.upper_bound(r->block.target);
        IFE_CHECK(nxt != m_dmg.end());
        if (nxt != m_dmg.end())
            IFE_CHECK(v != nullptr &&
                      v->repaired == static_cast<Offset>(nxt->first - r->block.target));
        // apply() writes the recomputed COUNT back; the file re-tiles.
        IFE_CHECK(rec.apply(rep));
        Recovery after({g.data(), g.size()});
        const FileMap m = after.scan();
        IFE_CHECK(m.gaps.empty());
    }
}

// ── RC-3.3 — both witnesses destroyed: the orphan, located and sized ──────

/// One victim per shape: plain block, array, byte array, nested triple. The
/// nested triple exists only in the 1.0 witness (its byte-run offsets name
/// child ATTRIBUTES blocks); the other shapes come from cipher_iris, which
/// covers every block.
struct Victim {
    MapEntryType type;
    Offset       off;
    Size         size;
    Offset       parent;   ///< of the reference to damage
    Size         slot;     ///< relative; the absolute slot is parent + slot
    const char*  what;
};

void collect_victims(std::vector<Victim>& __victims,
                     const RecoveryReport& __rep, const FileMap& __map,
                     MapEntryType __type, const char* __what) {
    for (const BlockVerdict& v : __rep.blocks) {
        if (v.block.expected != __type) continue;
        const Offset off = v.block.target;
        const auto it = __map.find(off);
        if (it == __map.end() || it->second.size == 0) continue;
        // Its run would be classed Trailing (nothing follows it), not Hole.
        if (std::next(__map.find(off)) == __map.end()) continue;
        // The two damage sites must be DISTINCT bytes: when the slot address
        // and the child's own offset coincide, two ^= 0xFF writes to one
        // byte cancel and the stream stays undamaged (the XOR-cancel guard).
        if (static_cast<Offset>(v.block.parent + v.block.slot) == off) continue;
        __victims.push_back({__type, off, it->second.size, v.block.parent,
                             v.block.slot, __what});
        return;  // one victim per shape
    }
}

void test_both_witnesses_orphan_located_and_sized() {
    const auto cipher = load_fixture("cipher_iris.test_slide");
    const auto v10    = load_fixture("v1_0_witness.test_slide");
    if (cipher.empty() || v10.empty()) return;

    Recovery clean_c({cipher.data(), cipher.size()});
    const RecoveryReport rep_c = clean_c.recover();
    const FileMap map_c = clean_c.scan();
    IFE_CHECK(rep_c.blocks_total > 0);
    IFE_CHECK(map_c.gaps.empty());

    Recovery clean_0({v10.data(), v10.size()});
    const RecoveryReport rep_0 = clean_0.recover();
    const FileMap map_0 = clean_0.scan();
    IFE_CHECK(map_0.gaps.empty());

    std::vector<Victim> victims;
    collect_victims(victims, rep_c, map_c,
                    MapEntryType::MAP_ENTRY_METADATA, "plain");       // FILE_HEADER -> METADATA
    collect_victims(victims, rep_c, map_c,
                    MapEntryType::MAP_ENTRY_LAYER_EXTENTS, "array");  // TILE_TABLE -> LAYER_EXTENTS
    collect_victims(victims, rep_c, map_c,
                    MapEntryType::MAP_ENTRY_ICC_PROFILE, "byte-array");  // METADATA -> ICC_PROFILE
    // The nested triple: a byte-run slot naming a child ATTRIBUTES block.
    // Such a ref has expected==ATTRIBUTES and its parent is itself an
    // ATTRIBUTES block (the METADATA->ATTRIBUTES ref has parent==METADATA).
    for (const BlockVerdict& v : rep_0.blocks) {
        if (v.block.expected != MapEntryType::MAP_ENTRY_ATTRIBUTES) continue;
        const auto pit = map_0.find(v.block.parent);
        if (pit == map_0.end() || pit->second.type != MapEntryType::MAP_ENTRY_ATTRIBUTES)
            continue;
        const auto it = map_0.find(v.block.target);
        if (it == map_0.end() || it->second.size == 0) continue;
        if (std::next(map_0.find(v.block.target)) == map_0.end()) continue;
        if (static_cast<Offset>(v.block.parent + v.block.slot) == v.block.target) continue;
        victims.push_back({MapEntryType::MAP_ENTRY_ATTRIBUTES, v.block.target,
                           it->second.size, v.block.parent, v.block.slot, "nested"});
        break;
    }
    IFE_CHECK(victims.size() >= 4);
    if (victims.size() < 4) return;

    std::printf("    orphaning victims:");
    for (const Victim& victim : victims)
        std::printf(" %s@%llu(%lluB)", victim.what,
                    (unsigned long long)victim.off, (unsigned long long)victim.size);
    std::printf("\n");

    for (const Victim& victim : victims) {
        auto f = (victim.type == MapEntryType::MAP_ENTRY_ATTRIBUTES) ? v10 : cipher;
        const std::string what = std::string(victim.what);

        f[victim.off] ^= 0xFF;                       // witness 2: child VALIDATION
        f[victim.parent + victim.slot] ^= 0xFF;      // witness 1: the parent slot

        Recovery rec({f.data(), f.size()});
        const FileMap damaged = rec.scan();
        // Precondition, asserted rather than assumed: if the block did not
        // actually vanish, everything below passes for the wrong reason.
        IFE_CHECK(!damaged.count(victim.off));

        const RecoveryReport rep = rec.recover();
        std::size_t holes = 0;
        bool exact = false;
        for (const Gap& g : rep.gaps)
            if (g.class_ == GapClass::Hole) {
                ++holes;
                if (g.start == victim.off && g.length == victim.size) exact = true;
            }
        IFE_CHECK(holes == 1);
        IFE_CHECK(exact);
    }
}

// ── RC-3.4 — frame survival rebuilds the tile entry ───────────────────────

void test_frame_survival_rebuilds_tile_entry() {
    auto f = load_fixture("v1_1_witness.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport base = clean.recover();

    // A framed tile edge: the walker's validation is the frame's 40-bit
    // self-offset, five bytes before the stream.
    const BlockVerdict* tref = nullptr;
    for (const BlockVerdict& v : base.blocks)
        if (v.block.expected == MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA &&
            v.block.validation != k::NULL_OFFSET) {
            tref = &v;
            break;
        }
    IFE_CHECK(tref != nullptr);
    if (!tref) return;
    const Offset stream = tref->block.target;
    const Size   stream_size = clean.scan().at(stream).size;
    IFE_CHECK(tref->block.validation ==
              static_cast<Offset>(static_cast<std::uint64_t>(stream) -
                                  b::TILE_PIXEL_DATA::size::VALIDATION));

    // Entry destroyed, frame survives: the frame-witnessed stream in the
    // census corroborates the true child, cost 1 (one flipped bit).
    {
        auto g = f;
        check_flip_took(g, tref->block.parent + tref->block.slot, true);
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, tref->block.parent, tref->block.slot);
        IFE_CHECK(v != nullptr && v->class_ == RepairClass::Corroborated);
        IFE_CHECK(v != nullptr && v->bit_cost == 1);
        IFE_CHECK(v != nullptr && v->repaired == stream);

        // apply() restores the entry — position AND the adjacent u24 SIZE
        // field survive (the repair writes a u40, not a u64).
        IFE_CHECK(rec.apply(rep));
        Recovery after({g.data(), g.size()});
        const FileMap m = after.scan();
        const auto it = m.find(stream);
        IFE_CHECK(it != m.end() &&
                  it->second.type == MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA &&
                  it->second.size == stream_size);
        IFE_CHECK(m.gaps.empty());
    }

    // Entry AND frame destroyed: no witness survives, so nothing is guessed —
    // the stream's run reads as a Hole.
    {
        auto g = f;
        check_flip_took(g, tref->block.parent + tref->block.slot, false);
        check_flip_took(g, stream - b::TILE_PIXEL_DATA::size::VALIDATION, false);
        Recovery rec({g.data(), g.size()});
        const FileMap damaged = rec.scan();
        IFE_CHECK(!damaged.count(stream));
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, tref->block.parent, tref->block.slot);
        IFE_CHECK(v != nullptr && v->class_ != RepairClass::Corroborated &&
                  v->class_ != RepairClass::PositionRepaired);
        IFE_CHECK(rep.holes >= 1);
    }

    // OFFSET AND SIZE both broken: the frame corroborates the position, but
    // the extent claim no longer ends at a boundary — the size validation
    // declines the repair (never guessed) and the run reads as a Hole.
    {
        auto g = f;
        check_flip_took(g, tref->block.parent + tref->block.slot, true);
        check_flip_took(g, tref->block.parent + tref->block.slot +
                               b::TILE_OFFSETS::TILE_OFFSET::size::OFFSET, false);
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, tref->block.parent, tref->block.slot);
        IFE_CHECK(v != nullptr && v->class_ == RepairClass::Unrecovered);
        IFE_CHECK(rep.holes >= 1);
    }
}

// ── RC-3.5 — never silent; red-green both ways ────────────────────────────

void test_never_silent() {
    auto f = load_fixture("v1_1_witness.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport base = clean.recover();

    // The frame's TILE_INDEX decides, where distance cannot. Two tile entries
    // corrupted to one value X equidistant from their two streams: on cost
    // alone this is a genuine tie and both edges abstain, which is what this
    // case asserted before the index was consulted. The frame says which
    // stream is which, so both edges now recover CORRECTLY — the ambiguity
    // was never in the file, only in the metric.
    {
        const BlockVerdict* r0 = nullptr;
        const BlockVerdict* r1 = nullptr;
        for (const BlockVerdict& v : base.blocks)
            if (v.block.expected == MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA) {
                if (!r0) r0 = &v;
                else if (v.block.target != r0->block.target) { r1 = &v; break; }
            }
        IFE_CHECK(r0 != nullptr && r1 != nullptr);
        if (!r0 || !r1) return;

        const std::uint64_t d = static_cast<std::uint64_t>(r0->block.target) ^
                                static_cast<std::uint64_t>(r1->block.target);
        std::uint64_t half = 0;
        int need = std::popcount(d) / 2;
        for (int bit = 0; need > 0; ++bit)
            if (d & (std::uint64_t{1} << bit)) { half |= std::uint64_t{1} << bit; --need; }
        const Offset X = static_cast<Offset>(static_cast<std::uint64_t>(r1->block.target) ^ half);

        auto g = f;
        ::Iris::File::store_u40(g.data() + r0->block.parent + r0->block.slot, X);
        ::Iris::File::store_u40(g.data() + r1->block.parent + r1->block.slot, X);
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        for (const BlockVerdict* r : {r0, r1}) {
            const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
            IFE_CHECK(v != nullptr && v->class_ == RepairClass::Corroborated);
            // The whole point: the RIGHT stream, not merely a confident one.
            IFE_CHECK(v != nullptr && v->repaired == r->block.target);
        }
    }

    // Ambiguous — never picked. The same two entries, but now BOTH frames
    // claim the same TILE_INDEX, so the identity witness is no evidence and
    // the engine is back on distance alone, where X is equidistant. Two live
    // readings: both candidates reported, nothing written. This is the case
    // the tile edge used to reach by construction; it now takes a file that
    // contradicts itself, which is the right price for an exact witness.
    {
        const BlockVerdict* r0 = nullptr;
        const BlockVerdict* r1 = nullptr;
        for (const BlockVerdict& v : base.blocks)
            if (v.block.expected == MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA) {
                if (!r0) r0 = &v;
                else if (v.block.target != r0->block.target) { r1 = &v; break; }
            }
        IFE_CHECK(r0 != nullptr && r1 != nullptr);
        if (!r0 || !r1) return;

        // X equidistant from both targets: flip half of the differing bits.
        const std::uint64_t d = static_cast<std::uint64_t>(r0->block.target) ^
                                static_cast<std::uint64_t>(r1->block.target);
        std::uint64_t half = 0;
        int need = std::popcount(d) / 2;
        for (int bit = 0; need > 0; ++bit)
            if (d & (std::uint64_t{1} << bit)) { half |= std::uint64_t{1} << bit; --need; }
        const Offset X = static_cast<Offset>(static_cast<std::uint64_t>(r1->block.target) ^ half);

        auto g = f;
        ::Iris::File::store_u40(g.data() + r0->block.parent + r0->block.slot, X);
        ::Iris::File::store_u40(g.data() + r1->block.parent + r1->block.slot, X);
        // Both streams claim r0's index, so neither is r1's and neither can be
        // told from the other. The frame grows BACKWARD from the stream's
        // first byte: TILE_INDEX is the four bytes below the five-byte
        // VALIDATION (CLAUDE.md, the tile-frame layout note).
        constexpr Size FRAME_BACK = ::Iris::File::primitives::FrameHeader::VALIDATION_SIZE
                                  + b::TILE_PIXEL_DATA::size::TILE_INDEX;
        const std::uint32_t claimed =
            ::Iris::File::load<std::uint32_t>(f.data() + r0->block.target - FRAME_BACK);
        ::Iris::File::store<std::uint32_t>(g.data() + r1->block.target - FRAME_BACK, claimed);
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v0 = find_verdict(rep, r0->block.parent, r0->block.slot);
        IFE_CHECK(v0 != nullptr && v0->class_ == RepairClass::Ambiguous);
        IFE_CHECK(v0 != nullptr && v0->candidates.size() == 2);
        IFE_CHECK(v0 != nullptr && v0->candidates.size() == 2 &&
                  (v0->candidates[0] == r0->block.target || v0->candidates[1] == r0->block.target) &&
                  (v0->candidates[0] == r1->block.target || v0->candidates[1] == r1->block.target));
        IFE_CHECK(v0 != nullptr && v0->repaired == k::NULL_OFFSET);  // never picked
    }

    // Unrecovered — reported, never dropped; the run still reads as a Hole.
    {
        const BlockVerdict* r0 = nullptr;
        for (const BlockVerdict& v : base.blocks)
            if (v.block.expected == MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA) { r0 = &v; break; }
        IFE_CHECK(r0 != nullptr);
        if (!r0) return;
        auto g = f;
        // Far beyond the flip budget of any stream: 24 set bits.
        ::Iris::File::store_u40(g.data() + r0->block.parent + r0->block.slot, 0x00FFFFFFu);
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, r0->block.parent, r0->block.slot);
        IFE_CHECK(v != nullptr && v->class_ == RepairClass::Unrecovered);
        IFE_CHECK(v != nullptr && v->repaired == k::NULL_OFFSET);
        IFE_CHECK(rep.holes >= 1);
    }

    // apply() on a zero-repair report mutates nothing — byte-identical.
    {
        auto g = f;
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        IFE_CHECK(rep.intact == rep.blocks_total);
        const std::vector<BYTE> snapshot = g;
        IFE_CHECK(rec.apply(rep));
        IFE_CHECK(g == snapshot);
    }

    // Both halves of one non-tile edge damaged: never reported Intact, never
    // silently dropped — the child is invisible to scan and unreachable, so
    // the verdict is Unrecovered, never a guess.
    {
        const BlockVerdict* r = find_ref(base, MapEntryType::MAP_ENTRY_IMAGES);
        IFE_CHECK(r != nullptr && r->class_ == RepairClass::Intact);
        if (!r || r->class_ != RepairClass::Intact) return;
        auto g = f;
        g[r->block.target] ^= 0x01;                       // child VALIDATION
        g[r->block.parent + r->block.slot + 7] ^= 0x80;   // parent slot MSB
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
        IFE_CHECK(v != nullptr && v->class_ != RepairClass::Intact);
    }
}


// ── RC-2.4 — a hole is a position candidate ──────────────────────────────

/// Both witnesses destroyed and NO surviving orphan: the only evidence the
/// child ever existed is the run of bytes nothing claims. RC-2.4 offers that
/// run to the ranker, so the parent's pointer is restorable even though the
/// child is not — and the two repairs chain: pointer first (HoleCorroborated),
/// then the child's own VALIDATION from the restored parent-named address
/// (PositionRepaired), after which the file re-tiles with no hole at all.
void test_hole_is_a_position_candidate() {
    auto f = load_fixture("cipher_iris.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport base = clean.recover();
    const FileMap map = clean.scan();

    const BlockVerdict* r = find_ref(base, MapEntryType::MAP_ENTRY_METADATA);
    IFE_CHECK(r != nullptr && r->class_ == RepairClass::Intact);
    if (!r || r->class_ != RepairClass::Intact) return;
    const Offset victim = r->block.target;
    const auto it = map.find(victim);
    IFE_CHECK(it != map.end() && it->second.size != 0);
    if (it == map.end() || it->second.size == 0) return;
    const Size victim_size = it->second.size;

    // The hole evidence is now a ranked self-signature sweep (REC-20.2,
    // ported RC-7), and the flip budget governs the WHOLE repair — offset
    // distance + the candidate's own residual damage must fit in
    // IFE_RECOVERY_MAX_FLIPS. Two damage sizes pin the two sides of that
    // rule:

    // CASE A — whole-byte VALIDATION damage (8 flips): the residual word is
    // located and the hole is sized, but repairing the parent edge would cost
    // 8 + 1 > the budget. The engine declines — honest Unrecovered, nothing
    // written — never a guess.
    {
        auto g = f;
        check_flip_took(g, victim, false);              // witness 2: VALIDATION
        check_flip_took(g, r->block.parent + r->block.slot, true);  // witness 1: ONE slot bit
        Recovery rec({g.data(), g.size()});
        const FileMap damaged = rec.scan();
        IFE_CHECK(!damaged.count(victim));              // the block really did vanish
        const RecoveryReport rep = rec.recover();

        std::size_t holes = 0;
        bool exact = false;
        for (const Gap& gap : rep.gaps)
            if (gap.class_ == GapClass::Hole) {
                ++holes;
                if (gap.start == victim && gap.length == victim_size) exact = true;
            }
        IFE_CHECK(holes == 1);
        IFE_CHECK(exact);

        const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
        IFE_CHECK(v != nullptr && v->class_ == RepairClass::Unrecovered);
        IFE_CHECK(rec.apply(rep));                      // nothing in budget — no write
        Recovery after({g.data(), g.size()});
        const FileMap m = after.scan();
        IFE_CHECK(!m.count(victim));                    // still gone: untouched
    }

    // CASE B — one-bit VALIDATION damage (the realistic shape of a flip):
    // the residual word at `victim` is 1 flip from its own address, so the
    // hole ranks at cost 2 (1 offset + 1 self) and the parent edge is
    // restored to it. The repair is HoleCorroborated — strictly weaker
    // evidence than a repair against a surviving block, counted apart — and
    // the bytes it names are still destroyed until a second pass heals them.
    {
        auto g = f;
        check_flip_took(g, victim, true);               // witness 2: ONE VALIDATION bit
        check_flip_took(g, r->block.parent + r->block.slot, true);  // witness 1: ONE slot bit
        Recovery rec({g.data(), g.size()});
        // The census locates the vanished block before any repair runs.
        const FileMap damaged = rec.scan();
        bool located = false;
        for (const Gap& gap : damaged.gaps)
            if (gap.class_ == GapClass::Hole && gap.start == victim &&
                gap.length == victim_size)
                located = true;
        IFE_CHECK(located);

        const RecoveryReport rep = rec.recover();
        // `holes` means "still missing AFTER recovery": the repair admitted
        // the block's run back into the census, so nothing is missing now.
        IFE_CHECK(rep.holes == 0);

        const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
        IFE_CHECK(v != nullptr && v->class_ == RepairClass::HoleCorroborated);
        IFE_CHECK(v != nullptr && v->bit_cost == 2);    // offset term 1 + self term 1
        IFE_CHECK(v != nullptr && v->repaired == victim);
        IFE_CHECK(rep.hole_corroborated == 1);
        // Never conflated with a repair made against a surviving block.
        IFE_CHECK(rep.corroborated == 0);

        // Step 1: the pointer comes back — AND the block's own header comes
        // with it. The winner-header audit classifies the restored block
        // while the slot repair is still being planned, so the child's
        // VALIDATION word (the second half of the 10-byte pair) is written in
        // the SAME apply(). The file is healed in one pass; there is no
        // second pass left to heal it.
        IFE_CHECK(rec.apply(rep));
        Recovery after({g.data(), g.size()});
        const FileMap m1 = after.scan();
        IFE_CHECK(m1.gaps.empty());
        const RecoveryReport rep2 = after.recover();
        const BlockVerdict* v2 = find_verdict(rep2, r->block.parent, r->block.slot);
        IFE_CHECK(v2 != nullptr && v2->block.target == victim);
        IFE_CHECK(v2 != nullptr && v2->class_ == RepairClass::Intact);
        IFE_CHECK(rep2.intact == rep2.blocks_total);
        IFE_CHECK(rep2.holes == 0);
    }
}

// ── Bounds: wire offsets are not trusted arithmetic ───────────────────────

/// A slot near 2^64 must not wrap past a bounds check. NULL_OFFSET is
/// all-ones and is what every unused slot holds, so ONE flipped bit there
/// produces 0xFF…FE — and `0xFF…FE + 10 <= size` is true. The engine read
/// eight bytes from before the mapping (ASan, 2026-08-28); on an mmap'd slide
/// the base is page-aligned, so that read is on the previous page. The
/// assertions below pin the visible behaviour; the memory error itself is
/// only observable under a sanitizer, which is what ife_damage_sweep_tests
/// exists to feed.
void test_wire_offsets_never_wrap_a_bounds_check() {
    auto f = load_fixture("v1_1_witness.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport base = clean.recover();

    // CIPHER_OFFSET is NULL_OFFSET in every fixture to date (RC-1.1's table).
    const Offset table = 38;
    const Size   slot  = b::TILE_TABLE::offset::CIPHER_OFFSET;
    IFE_CHECK(find_verdict(base, table, slot) == nullptr);   // absent, not an edge
    {
        auto g = f;
        IFE_CHECK(::Iris::File::load<std::uint64_t>(g.data() + table + slot) == k::NULL_OFFSET);
        check_flip_took(g, table + slot, true);              // -> 0xFF...FE
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, table, slot);
        // Now a (damaged) edge: enumerated and reported, never dereferenced.
        IFE_CHECK(v != nullptr);
        IFE_CHECK(v == nullptr || v->class_ == RepairClass::Unrecovered);
        IFE_CHECK(v == nullptr || v->repaired == k::NULL_OFFSET);
        const std::vector<BYTE> snapshot = g;
        IFE_CHECK(rec.apply(rep));
        IFE_CHECK(g == snapshot);                            // nothing to write
    }

    // The tile branch subtracts five to find the frame. A stream offset below
    // five underflows to all-ones — the value an ABSENT witness holds — so
    // without the guard a tile entry corrupted to 4 reads back as Intact.
    {
        const BlockVerdict* r = find_ref(base, MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA);
        IFE_CHECK(r != nullptr);
        if (!r) return;
        auto g = f;
        ::Iris::File::store_u40(g.data() + r->block.parent + r->block.slot, 4);
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
        IFE_CHECK(v != nullptr && v->class_ != RepairClass::Intact);
    }
}

// ── apply() is all or nothing ─────────────────────────────────────────────

/// The header promises "nothing is applied" when a write is out of range.
/// It used to write as it went and refuse afterwards, which left the file
/// half repaired — the one state the contract exists to prevent. The report
/// below is an engine-produced repair followed by an out-of-range one.
void test_apply_writes_nothing_when_any_repair_is_out_of_range() {
    auto f = load_fixture("v1_1_witness.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport base = clean.recover();
    const BlockVerdict* r = find_ref(base, MapEntryType::MAP_ENTRY_ICC_PROFILE);
    IFE_CHECK(r != nullptr && r->class_ == RepairClass::Intact);
    if (!r || r->class_ != RepairClass::Intact) return;

    // A real repair that apply() may always write: damage the child's
    // VALIDATION and let recover() class it PositionRepaired. (A TagRepaired
    // verdict is deliberately refused by apply() whenever the wire tag is
    // still a plausible type — its own gate, pinned in
    // test_tag_rewrite_gate below — so it is not usable as the "good"
    // repair of an all-or-nothing test.)
    check_flip_took(f, r->block.target, true);
    Recovery rec({f.data(), f.size()});
    RecoveryReport rep = rec.recover();
    const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
    IFE_CHECK(v != nullptr && v->class_ == RepairClass::PositionRepaired);
    if (!v || v->class_ != RepairClass::PositionRepaired) return;

    // Append one repair that cannot be written: its target is two bytes short
    // of EOF, so the u64 VALIDATION store would run past the end.
    BlockVerdict out_of_range;
    out_of_range.block.parent   = r->block.parent;
    out_of_range.block.slot     = r->block.slot;
    out_of_range.block.expected = MapEntryType::MAP_ENTRY_ICC_PROFILE;
    out_of_range.block.target   = static_cast<Offset>(f.size() - 2);
    out_of_range.class_         = RepairClass::PositionRepaired;
    rep.blocks.push_back(out_of_range);

    const std::vector<BYTE> snapshot = f;
    IFE_CHECK(!rec.apply(rep));
    IFE_CHECK(f == snapshot);          // the good repair must NOT have landed

    // Without the bad verdict the same report applies cleanly — the refusal
    // above is the range check, not a broken repair.
    rep.blocks.pop_back();
    IFE_CHECK(rec.apply(rep));
    IFE_CHECK(f != snapshot);
}

// ── RC-7 — apply() rewrites a type only when the wire tag is implausible ──

/// TagRepaired is decided on "the child validates but its tag disagrees",
/// which is two situations wearing one face: the child's TAG was flipped, or
/// the PARENT's offset was flipped onto an innocent, perfectly valid block of
/// another type. The ranker picks whichever is cheaper in bits and is
/// sometimes wrong — which costs nothing while READING, but while WRITING a
/// wrong guess relabels real data and removes that block's whole subtree
/// from the census permanently (recovery_handoff item 18; FastFHIR measured
/// applying every tag rewrite made its stream strictly worse). So apply()
/// writes a tag rewrite ONLY when the tag on the wire is already
/// implausible — no innocent type left to destroy; a plausible-but-different
/// tag stays a report.
void test_tag_rewrite_gate() {
    auto f = load_fixture("v1_1_witness.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport base = clean.recover();
    const BlockVerdict* r = find_ref(base, MapEntryType::MAP_ENTRY_ICC_PROFILE);
    IFE_CHECK(r != nullptr && r->class_ == RepairClass::Intact);
    if (!r || r->class_ != RepairClass::Intact) return;
    const Offset tag_at = r->block.target + b::ICC_PROFILE::offset::RECOVERY;
    const std::uint16_t declared = static_cast<std::uint16_t>(
        Abstraction::recovery_for(r->block.expected));

    // The gate: a tag that is still a plausible type could be an innocent
    // block's real type — reported, never rewritten.
    {
        auto g = f;  // wire tag intact and plausible
        Recovery rec({g.data(), g.size()});
        BlockVerdict v = *r;              // the Intact verdict, re-declared
        v.class_ = RepairClass::TagRepaired;
        RecoveryReport rep;
        rep.blocks.push_back(v);
        const std::vector<BYTE> snapshot = g;
        IFE_CHECK(rec.apply(rep));
        IFE_CHECK(g == snapshot);         // declined: nothing written
    }

    // A tag with NO plausible type has nothing to destroy: written back.
    {
        auto g = f;
        ::Iris::File::store<std::uint16_t>(g.data() + tag_at, 0xFFFFu);  // unassigned — not a type
        Recovery rec({g.data(), g.size()});
        BlockVerdict v;
        v.block = r->block;
        v.class_ = RepairClass::TagRepaired;
        RecoveryReport rep;
        rep.blocks.push_back(v);
        IFE_CHECK(rec.apply(rep));
        IFE_CHECK(::Iris::File::load<std::uint16_t>(g.data() + tag_at) == declared);
    }
}

// ── RC-8 — tag consensus (FastFHIR REC-22.2): coherence is the third opinion ─

/// A child that validates but carries a tag of ANOTHER plausible type is two
/// situations wearing one face: its TAG was flipped, or the parent's offset
/// landed on an innocent valid block of another type. Cost cannot separate
/// them (both hypotheses are one bit); COHERENCE can — does the block read as
/// the slot's declared type? This flips ONE bit of the child's wire tag on a
/// block that owns children (so block_reads_as has slots to judge by) and
/// asserts the consensus is decisive — TagRepaired, adjudicated — and NEVER
/// wrong, and that apply() may land the rewrite even though the wire tag is a
/// plausible type, because evidence rather than a plausibility guess decided
/// it. The flip never touches the block's own slots, so the bytes still read
/// as their declared type: the child's header is unambiguously the damaged
/// copy (FastFHIR's test flips both halves and asserts consensus is decisive
/// in either direction; IFE slots carry no tag half, so the child header is
/// the only wire copy there is to flip).
void test_tag_consensus_resolves_flipped_child_tag() {
    auto f = load_fixture("cipher_iris.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport baseline = clean.recover();

    // Blocks that OWN children — only they give block_reads_as a batch to
    // judge by (a block with no enumerable children answers neither way).
    std::map<Offset, std::size_t> outgoing;
    for (const BlockVerdict& v : baseline.blocks)
        if (v.block.target != k::NULL_OFFSET) ++outgoing[v.block.parent];

    std::size_t tried = 0, decisive = 0, wrong = 0;
    for (const BlockVerdict& cv : baseline.blocks) {
        if (cv.class_ != RepairClass::Intact) continue;
        const BlockRef r = cv.block;
        if (r.target == k::NULL_OFFSET ||
            r.expected == MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA ||
            r.expected == MapEntryType::MAP_ENTRY_FILE_HEADER)
            continue;
        if (!outgoing.count(r.target)) continue;  // no children — no opinion
        if (++tried > 24) break;

        auto g = f;
        const Offset seat = r.target + static_cast<Offset>(p::BlockHeader::RECOVERY);
        const std::uint16_t declared = static_cast<std::uint16_t>(
            Abstraction::recovery_for(r.expected));
        const std::uint16_t original = ::Iris::File::load<std::uint16_t>(g.data() + seat);

        // Flip ONE bit — but only a flip that lands on ANOTHER plausible type
        // is the case plausibility cannot decide (the flipped-to code could be
        // an innocent block's real type). Find that bit; a ref whose code has
        // no such neighbour cannot exercise consensus and is skipped.
        std::uint16_t mask = 0;
        for (unsigned bit = 0; bit < 16; ++bit) {
            const std::uint16_t cand = static_cast<std::uint16_t>(original ^ (1u << bit));
            const auto t = Abstraction::entry_for(
                static_cast<k::RecoveryCodes>(cand));
            if (t != MapEntryType::MAP_ENTRY_UNDEFINED && t != r.expected) {
                mask = static_cast<std::uint16_t>(1u << bit);
                break;
            }
        }
        if (mask == 0) continue;
        ::Iris::File::store<std::uint16_t>(g.data() + seat,
                                    static_cast<std::uint16_t>(original ^ mask));

        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        const BlockVerdict* v = find_verdict(rep, r.parent, r.slot);
        if (!v || v->class_ != RepairClass::TagRepaired || !v->tag_adjudicated)
            continue;  // Undecided on this flip — not a wrong answer
        ++decisive;
        // Never confidently wrong: the consensus must be the slot's declared
        // type — the bytes' own reading — never the flipped code.
        if (v->block.expected != r.expected) ++wrong;
        IFE_CHECK(v->bit_cost == 1);
        // The adjudicated rewrite may land even though the flipped-to code is
        // a plausible type (the write gate's "innocent block" worry is
        // answered by evidence), and the header must read as declared after.
        IFE_CHECK(rec.apply(rep));
        IFE_CHECK(::Iris::File::load<std::uint16_t>(g.data() + seat) == declared);
    }
    IFE_CHECK(tried > 0);   // the fixture contains blocks with children
    IFE_CHECK(wrong == 0);  // consensus is never confidently wrong
    IFE_CHECK(decisive > 0);
}

// ── Zero-coverage paths, closed 2026-08-28 ────────────────────────────────

/// The root's only self-evidence is its MAGIC (no VALIDATION by design),
/// but a magic within the flip budget is now repairable BY VIRTUE OF BEING
/// A HEADER: recover() rewrites the constant the format defines and the
/// walk runs again. scan() alone (the census view, before any repair is
/// applied) still shows the honest Hole at [0, root_extent) — the two entry
/// points answer different questions, and the hole is the census's way of
/// being honest about bytes no repair has been applied to yet.
void test_damaged_root_magic_is_repaired_within_budget() {
    auto f = load_fixture("cipher_iris.test_slide");
    if (f.empty()) return;
    f[0] ^= 0xFF;   // first MAGIC byte: 8 flips — the budget edge
    Recovery rec({f.data(), f.size()});
    const FileMap map = rec.scan();
    std::size_t holes = 0;
    bool at_root = false;
    for (const Gap& g : map.gaps)
        if (g.class_ == GapClass::Hole) {
            ++holes;
            if (g.start == 0 && g.length == b::FILE_HEADER::header_size) at_root = true;
        }
    IFE_CHECK(at_root);
    IFE_CHECK(holes == 1);
    const RecoveryReport rep = rec.recover();
    // Root repair: the MAGIC is a known constant, so recover() plans a
    // RootRepaired write, the walk admits the root run, and the honest hole
    // closes. The census remainder behaviour is unchanged for every block
    // BELOW the root — an orphaned parent's references are still real.
    IFE_CHECK(rep.blocks_total > 0);
    IFE_CHECK(rep.root_repaired == 1);
    IFE_CHECK(rep.holes == 0);
    // The repair lands: apply() restores the magic, and a fresh census both
    // claims the root and reports no hole at [0, root_extent).
    IFE_CHECK(rec.apply(rep));
    const FileMap after = Recovery({f.data(), f.size()}).scan();
    bool root_hole_after = false;
    for (const Gap& g : after.gaps)
        if (g.class_ == GapClass::Hole && g.start == 0) root_hole_after = true;
    IFE_CHECK(!root_hole_after);
    IFE_CHECK(after.count(0) == 1);
}

/// Damage BEYOND the flip budget is not guessed: a magic word trashed past
/// IFE_RECOVERY_MAX_FLIPS earns no RootRepaired write, and recover() keeps
/// the pre-repair degradation — the census alone, with the honest hole at
/// the root standing. When BOTH identity markers (magic AND the root tag)
/// are beyond budget, the identity gate fires: the bytes are not a lightly
/// damaged Iris file, nothing is fabricated, and the refusal is recorded.
void test_damaged_root_magic_beyond_budget_stays_holed() {
    auto f = load_fixture("cipher_iris.test_slide");
    if (f.empty()) return;
    f[0] = 0x00;   // magic byte 0: 0x73 -> 0x00 is 5 flips; with the other
    f[1] = 0x00;   // three bytes zeroed the word is ~26 flips from MAGIC_BYTES
    f[2] = 0x00;
    f[3] = 0x00;
    Recovery rec({f.data(), f.size()});
    const RecoveryReport rep = rec.recover();
    IFE_CHECK(rep.root_repaired == 0);
    IFE_CHECK(rep.holes == 1);

    // Identity gate: magic AND the root RECOVERY tag both destroyed. The
    // header is not repairable into Iris-ness — that would be fabrication.
    auto g = load_fixture("cipher_iris.test_slide");
    if (g.empty()) return;
    g[0] = 0x00; g[1] = 0x00; g[2] = 0x00; g[3] = 0x00;   // magic gone (16 flips)
    g[4] = 0xFF; g[5] = 0xFF;   // 0x5501 tag -> 0xFFFF: 11 flips — beyond budget too
    Recovery rec2({g.data(), g.size()});
    const RecoveryReport rep2 = rec2.recover();
    IFE_CHECK(rep2.root_repaired == 0);
    bool recorded = false;
    for (const ProducerFailure& pf : rep2.failures)
        if (pf.kind == ProducerFailureKind::NotAnIrisFile) recorded = true;
    IFE_CHECK(recorded);   // never silent
    IFE_CHECK(rep2.holes == 1);
}

/// Unframed stream (v1_0, no frames): the tile-offsets entry is the stream's
/// ONLY extent witness. Corrupt it and nothing can contradict the entry's
/// claim — the edge reports Intact (never guessed), and the stream's run
/// reads as a Hole at its exact position and size.
void test_unframed_tile_entry_corruption_is_a_hole() {
    auto f = load_fixture("v1_0_witness.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport base = clean.recover();
    const FileMap clean_map = clean.scan();
    const BlockVerdict* r = nullptr;
    for (const BlockVerdict& v : base.blocks)
        if (v.block.expected == MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA &&
            v.block.validation == k::NULL_OFFSET) {          // unframed
            const auto it = clean_map.find(v.block.target);
            if (it == clean_map.end()) continue;
            if (std::next(it) == clean_map.end()) continue;  // would be Trailing
            r = &v;
            break;
        }
    IFE_CHECK(r != nullptr);
    if (!r) return;
    const Offset stream = r->block.target;
    const Size   stream_size = clean_map.at(stream).size;

    auto g = f;
    ::Iris::File::store_u40(g.data() + r->block.parent + r->block.slot, 0xFFFFFFu);
    Recovery rec({g.data(), g.size()});
    const RecoveryReport rep = rec.recover();
    const BlockVerdict* v = find_verdict(rep, r->block.parent, r->block.slot);
    IFE_CHECK(v != nullptr && v->class_ == RepairClass::Intact);
    const FileMap damaged = rec.scan();
    IFE_CHECK(!damaged.count(stream));
    bool exact = false;
    for (const Gap& gap : rep.gaps)
        if (gap.class_ == GapClass::Hole && gap.start == stream &&
            gap.length == stream_size)
            exact = true;
    IFE_CHECK(exact);
}

/// The version gate's positive direction, and its safety-critical inverse.
/// v1_0 has FOUR ANNOTATION_BYTES instances; shrink every one's COUNT by 16
/// so each trails the same run — the shape a newer layout takes, and the
/// self-calibrating case (2+ instances, so the reader can tell a delta from
/// N one-off holes). Declared 1.2: benign VersionSkew, never damage.
/// Same bytes, declared 1.1: the SAME runs are damage — a same-version
/// stream must NEVER report skew (that would reclassify real damage as
/// benign, which is strictly worse than reporting nothing).
void test_version_skew_positive_and_never_on_same_version() {
    auto base = load_fixture("v1_0_witness.test_slide");
    if (base.empty()) return;
    Recovery clean({base.data(), base.size()});
    const FileMap map = clean.scan();
    std::vector<Offset> annots;
    for (const auto& [off, e] : map)
        if (e.type == MapEntryType::MAP_ENTRY_ANNOTATION_BYTES &&
            e.size > b::ANNOTATION_BYTES::header_size + 16)
            annots.push_back(off);
    IFE_CHECK(annots.size() >= 2);
    if (annots.size() < 2) return;

    auto shrink = [&](std::vector<BYTE>& f) {
        for (const Offset off : annots) {
            const Size count = map.at(off).size - b::ANNOTATION_BYTES::header_size;
            ::Iris::File::store<std::uint32_t>(f.data() + off + b::ANNOTATION_BYTES::offset::COUNT,
                                        static_cast<std::uint32_t>(count - 16));
        }
    };
    {   // declared 1.2: a systematic delta is benign — every instance trails
        // the same 16-byte run, self-calibrated, never a Hole
        auto f = base;
        ::Iris::File::store<std::uint16_t>(f.data() + b::FILE_HEADER::offset::EXTENSION_MINOR, 2);
        shrink(f);
        Recovery rec({f.data(), f.size()});
        const FileMap m = rec.scan();
        std::size_t skew = 0, holes = 0;
        bool all16 = true;
        for (const Gap& g : m.gaps) {
            if (g.class_ == GapClass::VersionSkew) ++skew;
            else if (g.class_ == GapClass::Hole) ++holes;
            if (g.length != 16) all16 = false;
        }
        IFE_CHECK(skew == annots.size() && holes == 0);
        IFE_CHECK(all16);
    }
    {   // same bytes, declared 1.1: the SAME runs are damage, never skew
        auto f = base;
        shrink(f);
        Recovery rec({f.data(), f.size()});
        const FileMap m = rec.scan();
        std::size_t skew = 0, holes = 0;
        for (const Gap& g : m.gaps) {
            if (g.class_ == GapClass::VersionSkew) ++skew;
            else if (g.class_ == GapClass::Hole) ++holes;
        }
        IFE_CHECK(skew == 0 && holes == annots.size());
    }
}

/// apply()'s bounds contract: any out-of-range write aborts the whole pass
/// — a partially repaired file is worse than an untouched one. Hand-built
/// verdicts exercise both the u64 slot path and the u40 tile-slot path.
void test_apply_out_of_range_aborts() {
    auto f = load_fixture("cipher_iris.test_slide");
    if (f.empty()) return;
    Recovery rec({f.data(), f.size()});

    BlockVerdict v64;   // u64 slot write beyond EOF (PositionRepaired)
    v64.block.expected = MapEntryType::MAP_ENTRY_TILE_TABLE;
    v64.block.target   = 0xFFFFFF;
    v64.class_         = RepairClass::PositionRepaired;
    BlockVerdict vt;    // u40 tile-slot write beyond EOF (Corroborated)
    vt.block.expected = MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA;
    vt.block.parent   = 0xFFFFFF;
    vt.class_         = RepairClass::Corroborated;
    vt.repaired       = 0;

    for (const BlockVerdict& v : {v64, vt}) {
        RecoveryReport rep;
        rep.blocks.push_back(v);
        auto g = f;
        Recovery r2({g.data(), g.size()});
        IFE_CHECK(!r2.apply(rep));
        IFE_CHECK(g == f);   // nothing applied
    }
}

/// A file too small to hold even a frame signature: the scan degrades to the
/// gap sweep over an empty map, and recover() reports nothing — no crash.
void test_tiny_file_scan_is_safe() {
    const BYTE bytes[3] = {0x00, 0x55, 0x01};
    Recovery rec({bytes, 3});
    const FileMap map = rec.scan();
    IFE_CHECK(map.empty());
    IFE_CHECK(map.gaps.empty());
    const RecoveryReport rep = rec.recover();
    IFE_CHECK(rep.blocks_total == 0 && rep.holes == 0);
}

// ── RC-3.6 — the REC-19 port: one damaged witness costs nothing ───────────

/// The whole-report shape, not one verdict. The REC-19 defect was a loss that
/// could not be seen in the report it was absent from: one flipped bit in a
/// block's VALIDATION cost every reference below it while recover() reported
/// zero failures, because the references were never enumerated to be
/// repaired. Both halves of the child header are exercised:
///
///   * the VALIDATION flip — the walk's flip-budget descent still reaches
///     the block, its subtree is enumerated, and the parent-attested
///     admission closes the run (no hole opens);
///   * the RECOVERY-tag flip — the walk refuses to descend (the tag does not
///     corroborate), the edge is TagRepaired, and the REC-19.7 reapply
///     re-enumerates the subtree under the corrected type.
static void test_one_damaged_witness_costs_nothing() {
    auto f = load_fixture("cipher_iris.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport base = clean.recover();
    IFE_CHECK(base.holes == 0 && base.unrecovered == 0 && base.ambiguous == 0);
    IFE_CHECK(base.intact == base.blocks_total);

    // A block WITH children to lose: damaging a leaf would pass even with
    // the walk truncated. `outgoing` = the refs it owns, `children` = refs
    // that name it.
    std::map<Offset, std::size_t> children, outgoing;
    for (const BlockVerdict& v : base.blocks) {
        if (v.block.target != k::NULL_OFFSET) ++children[v.block.target];
        ++outgoing[v.block.parent];
    }
    Offset victim = k::NULL_OFFSET;
    for (const BlockVerdict& v : base.blocks) {
        const Offset off = v.block.target;
        if (off == k::NULL_OFFSET || off == 0) continue;
        if (outgoing[off] >= 2 && children.count(off)) { victim = off; break; }
    }
    IFE_CHECK(victim != k::NULL_OFFSET);
    if (victim == k::NULL_OFFSET) return;

    // Witness 2a — the child's own self-offset. ONE bit, so the walk's
    // flip-budget descent still reaches the block, the parent's type
    // corroboration admits it to the census map, and nothing below it is
    // lost. The audit records the InvalidSelfRef — the loss is visible even
    // though every verdict comes out repairable.
    {
        auto g = f;
        check_flip_took(g, victim, true);
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        IFE_CHECK(rep.blocks_total == base.blocks_total);
        IFE_CHECK(rep.holes == 0);
        IFE_CHECK(rep.unrecovered == 0);
        IFE_CHECK(rep.ambiguous == 0);
        IFE_CHECK(rep.position_repaired >= 1);
        bool audited = false;
        for (const ProducerFailure& fl : rep.failures)
            if (fl.kind == ProducerFailureKind::InvalidSelfRef && fl.at == victim)
                audited = true;
        IFE_CHECK(audited);
    }

    // Witness 2b — the child's RECOVERY tag. The walk refuses to descend
    // (the tag no longer corroborates the slot's type), the edge is
    // TagRepaired, and the reapply enumerates the subtree under the
    // corrected type — the report is complete without apply() having run.
    // (The hole state is not pinned here: the census notes the flipped tag
    // under whatever type it now maps to, so the tiling depends on which
    // tag the flip landed on — the reference count does not.)
    {
        auto g = f;
        check_flip_took(g, victim + static_cast<Offset>(p::BlockHeader::RECOVERY), true);
        Recovery rec({g.data(), g.size()});
        const RecoveryReport rep = rec.recover();
        IFE_CHECK(rep.blocks_total == base.blocks_total);
        IFE_CHECK(rep.tag_repaired >= 1);
        IFE_CHECK(rep.unrecovered == 0);
        bool audited = false;
        for (const ProducerFailure& fl : rep.failures)
            if (fl.kind == ProducerFailureKind::VTableRecoveryMismatch && fl.at == victim)
                audited = true;
        IFE_CHECK(audited);
    }
}

// ── RC-7 — the generational cascade (FastFHIR REC-20, recovery_handoff
// items 1, 7, 16, 21): three generations, both witnesses destroyed at each,
// recover from the root — and the assertions are made against a KNOWN
// denominator, never against whatever the previous run happened to produce.

/// One three-generation cascade trial. `__mask` is the damage applied to each
/// child's VALIDATION word: 0x01 (one flip — the tight-band pool finds the
/// holes on the first pass) or 0x07 (three flips — only the progressive band
/// expansion can see them; recovery_handoff item 15: at 2 bits the signature
/// is a clean signal across the arena and BLIND to a block whose VALIDATION
/// took 3 or more flips, and widening is safe only late, against a pool
/// earlier rounds have emptied).
void run_generational_trial(const char* __fixture, std::uint8_t __mask) {
    auto f = load_fixture(__fixture);
    if (f.empty()) return;

    // The known denominator, captured BEFORE any damage (item 1): in a test
    // the corruption is chosen, so the answer is known before the run — a
    // number going up must not read as progress when it is an accounting
    // change.
    Recovery clean({f.data(), f.size()});
    const RecoveryReport baseline = clean.recover();
    IFE_CHECK(baseline.holes == 0);
    if (baseline.holes != 0) return;
    const std::size_t expected_refs = baseline.blocks_total;

    // A chain member must point at a HEADERED child (item 9: arrays and byte
    // arrays are not datablocks; streams have no header at all — a tile
    // stream's witnesses are the entry and the optional frame, and "destroy
    // both witnesses" cannot be expressed for a headerless run). Only a
    // header-carrying block can vanish from the census and re-enter it as a
    // hole bounded by its neighbours.
    const auto datablock_child = [](const BlockRef& r) {
        return r.target != k::NULL_OFFSET &&
               r.expected != MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA;
    };

    // Find a three-generation chain of pointed-to blocks: gen1 names a child
    // that is itself a parent, and so on. Indexed by parent so the descent is
    // a lookup.
    std::map<Offset, std::vector<BlockRef>> by_parent;
    for (const BlockVerdict& v : baseline.blocks)
        if (datablock_child(v.block))
            by_parent[v.block.parent].push_back(v.block);

    BlockRef gen1{}, gen2{}, gen3{};
    bool found = false;
    for (const BlockVerdict& v1 : baseline.blocks) {
        const BlockRef a = v1.block;
        if (!datablock_child(a)) continue;
        if (!by_parent.count(a.target)) continue;
        for (const BlockRef& b : by_parent[a.target]) {
            if (!datablock_child(b) || !by_parent.count(b.target)) continue;
            for (const BlockRef& c : by_parent[b.target]) {
                if (!datablock_child(c)) continue;
                // Three distinct blocks: three separate holes, not one block
                // damaged three times.
                if (a.target == b.target || b.target == c.target || a.target == c.target)
                    continue;
                // TWO WITNESSES, NOT ONE (item 21): an inline element lives AT
                // the slot that names it — parent + slot == child — so it has
                // a single witness and "destroy both" would flip the same
                // byte twice and cancel. Guard first, and let the test fail
                // loudly rather than silently test nothing.
                const auto two_witnesses = [](const BlockRef& r) {
                    return r.parent + r.slot != r.target;
                };
                if (!two_witnesses(a) || !two_witnesses(b) || !two_witnesses(c))
                    continue;
                gen1 = a; gen2 = b; gen3 = c; found = true;
                break;
            }
            if (found) break;
        }
        if (found) break;
    }
    IFE_CHECK(found);
    if (!found) return;

    // FULL HOLES at every level: the parent's stored offset (witness 1, ONE
    // bit) and the child's VALIDATION word (witness 2, `__mask`).
    const BlockRef chain[3] = {gen1, gen2, gen3};
    for (const BlockRef& r : chain) {
        check_flip_took(f, r.parent + r.slot, true);
        const Offset at = r.target;
        const BYTE before = f[at];
        f[at] ^= __mask;
        IFE_CHECK(f[at] != before);
    }

    Recovery rec({f.data(), f.size()});
    const RecoveryReport rep = rec.recover();

    // The assertions are against the known quantity. This is the test that
    // found four real defects in its first hour in FastFHIR — the coherence
    // gate silencing the cascade (item 7), size-equality hole matching,
    // extent-as-emission-bound, and the single-witness inline element.
    IFE_CHECK(rep.blocks_total == expected_refs);  // every reference is accounted for
    IFE_CHECK(rep.holes == 0);                     // no hole survives recovery
    IFE_CHECK(rep.unrecovered == 0);               // nothing is left unrecovered
    IFE_CHECK(rep.ambiguous == 0);                 // nothing is left ambiguous

    // And specifically that the DEEPER generations came back — an engine that
    // repairs only the first level still satisfies a naive reference count
    // when the subtree happens to be small. A repaired reference still
    // carries the CORRUPTED offset in block.target — the repair is the
    // candidate the ranker chose (repaired/candidates), so checking
    // block.target alone would miss every successful repoint, which is
    // exactly the outcome being asserted.
    const auto reached = [&rep](Offset off) {
        for (const BlockVerdict& v : rep.blocks) {
            if (v.block.target == off) return true;
            if (v.repaired == off) return true;
            for (const Offset cand : v.candidates)
                if (cand == off) return true;
        }
        return false;
    };
    IFE_CHECK(reached(gen2.target));
    IFE_CHECK(reached(gen3.target));
}

void test_generational_holes_recover_from_the_root() {
    // The realistic one-bit shape of a flip: the tight band-2 pool finds all
    // three generations, and the reapply unwinds the cascade from the root.
    run_generational_trial("cipher_iris.test_slide", 0x01);
}

void test_band_widening_closes_deeper_validation_damage() {
    // Three flips on every VALIDATION word: invisible at the tight band,
    // recovered only by the progressive band expansion (item 15).
    run_generational_trial("cipher_iris.test_slide", 0x07);
}

}  // namespace

// ── RC-9 — exclusivity: two references cannot own one child ───────────────
//
// The repoint ranker picks per reference and greedily. Two damaged slots can
// therefore both land on the cheapest surviving block of their type, and both
// report Corroborated with full confidence — one of them attached to a child
// that is demonstrably somebody else's. FastFHIR traced one of eleven
// mis-attachments to exactly this (REC-23.2); the cost is not a lost
// reference but a plausible wrong one, which nothing downstream can detect.
//
// Construction: take two same-typed edges, and corrupt BOTH slots to one-bit
// neighbours of the SAME child. Each slot then ranks that child cheapest, and
// nothing local says which is entitled to it.
void test_contended_repoint_is_never_confident() {
    auto f = load_fixture("v1_0_witness.test_slide");
    if (f.empty()) return;
    Recovery clean({f.data(), f.size()});
    const RecoveryReport base = clean.recover();

    // Two edges of one type, from one parent, whose slots are plain u64
    // offsets — the tile edge is excluded on purpose: its frame carries an
    // exact identity witness, so it cannot be made to contend.
    const BlockVerdict* a = nullptr;
    const BlockVerdict* b = nullptr;
    const auto usable = [](const BlockVerdict& v) {
        return v.class_ == RepairClass::Intact &&
               v.block.expected != MapEntryType::MAP_ENTRY_TILE_PIXEL_DATA;
    };
    for (const BlockVerdict& x : base.blocks) {
        if (!usable(x)) continue;
        for (const BlockVerdict& y : base.blocks) {
            if (&y == &x || !usable(y)) continue;
            if (y.block.expected != x.block.expected) continue;
            if (y.block.parent != x.block.parent) continue;
            if (y.block.target == x.block.target) continue;
            a = &x; b = &y;
            break;
        }
        if (a) break;
    }
    IFE_CHECK(a != nullptr && b != nullptr);
    if (!a || !b) return;

    // Both slots corrupted to distinct one-bit neighbours of b's child, so
    // b's child is the cheapest candidate for each. a's child is orphaned by
    // the same edit and remains in the pool — the tie is over b's child.
    auto g = f;
    ::Iris::File::store<std::uint64_t>(g.data() + a->block.parent + a->block.slot,
                                static_cast<std::uint64_t>(b->block.target) ^ 1u);
    ::Iris::File::store<std::uint64_t>(g.data() + b->block.parent + b->block.slot,
                                static_cast<std::uint64_t>(b->block.target) ^ 2u);

    Recovery rec({g.data(), g.size()});
    const RecoveryReport rep = rec.recover();
    const BlockVerdict* va = find_verdict(rep, a->block.parent, a->block.slot);
    const BlockVerdict* vb = find_verdict(rep, b->block.parent, b->block.slot);
    IFE_CHECK(va != nullptr && vb != nullptr);
    if (!va || !vb) return;

    // Neither may claim it confidently, and neither may be written.
    const bool a_took = va->class_ == RepairClass::Corroborated ||
                        va->class_ == RepairClass::HoleCorroborated;
    const bool b_took = vb->class_ == RepairClass::Corroborated ||
                        vb->class_ == RepairClass::HoleCorroborated;
    IFE_CHECK(!(a_took && va->repaired == b->block.target &&
                b_took && vb->repaired == b->block.target));
    if (va->repaired != k::NULL_OFFSET && vb->repaired != k::NULL_OFFSET)
        IFE_CHECK(va->repaired != vb->repaired);

    // The demoted verdict still reports what it wanted: a driver has to be
    // able to see the contention, which a bare Ambiguous would hide.
    for (const BlockVerdict* v : {va, vb})
        if (v->class_ == RepairClass::Ambiguous)
            IFE_CHECK(!v->candidates.empty() && v->repaired == k::NULL_OFFSET);

    // The control: apply() must not write a demoted verdict. Ambiguous is
    // reported precisely because the engine declined to choose.
    auto h = g;
    Recovery mut({h.data(), h.size()});
    mut.apply(rep);
    for (const BlockVerdict* v : {va, vb})
        if (v->class_ == RepairClass::Ambiguous)
            IFE_CHECK(::Iris::File::load<std::uint64_t>(h.data() + v->block.parent + v->block.slot) ==
                      ::Iris::File::load<std::uint64_t>(g.data() + v->block.parent + v->block.slot));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 2;
    }
    g_corpus_dir = ife_corpus_dir(argv[1]);

    test_clean_stream_zero_damage();        // RC-3.1
    test_one_witness_per_class();           // RC-3.2
    test_both_witnesses_orphan_located_and_sized();  // RC-3.3
    test_frame_survival_rebuilds_tile_entry();       // RC-3.4
    test_never_silent();                    // RC-3.5
    test_damaged_root_magic_is_repaired_within_budget();
    test_damaged_root_magic_beyond_budget_stays_holed();
    test_unframed_tile_entry_corruption_is_a_hole();
    test_version_skew_positive_and_never_on_same_version();
    test_apply_out_of_range_aborts();
    test_tiny_file_scan_is_safe();
    test_hole_is_a_position_candidate();    // RC-2.4
    test_wire_offsets_never_wrap_a_bounds_check();
    test_apply_writes_nothing_when_any_repair_is_out_of_range();
    test_tag_rewrite_gate();                 // RC-7 (REC-15 write gate)
    test_tag_consensus_resolves_flipped_child_tag();  // RC-8 (REC-22.2)
    test_one_damaged_witness_costs_nothing();  // RC-3.6 (REC-19 port)
    test_generational_holes_recover_from_the_root();  // RC-7 (REC-20 port)
    test_band_widening_closes_deeper_validation_damage();  // RC-7 (REC-20.5)
    test_contended_repoint_is_never_confident();  // RC-9 (REC-23.2)

    if (g_failures) {
        std::fprintf(stderr, "ife_recovery_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ife_recovery_tests: all RC-3 checks passed\n");
    return 0;
}
