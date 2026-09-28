/**
 * @file ife_advanced_api_tests.cpp
 * @brief The ADVANCED tier (IFE_Advanced.hpp): the recovery / modification
 *        surface — the file map, the gap census, the per-reference verdicts,
 *        and the two entry points that produce them.
 *
 * Its counterpart, ife_api_contract_tests.cpp, proves the PUBLIC tier
 * (IrisFileExtension.hpp) carries the READ surface alone. Together they are
 * the tier boundary: this file includes the advanced header; that one does
 * not, and could not name FileMap/BlockVerdict/RecoveryReport.
 *
 * SELF-CONTAINED (no external framework). Non-zero exit on failure.
 */
#include "IFE_Advanced.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <type_traits>
#include <utility>

using namespace Iris::File;

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

// ---------------------------------------------------------------------------
// The advanced entry points and their return type.
// ---------------------------------------------------------------------------
static_assert(std::is_same_v<decltype(generate_file_map(std::declval<const FileAccessInfo&>())), Abstraction::FileMap>);
static_assert(std::is_same_v<decltype(recover_file_structure(std::declval<const FileAccessInfo&>())), Abstraction::FileMap>);

// The advanced global aliases mirror their namespaced types.
static_assert(std::is_same_v<IFE_MapEntryType, Abstraction::MapEntryType>);
static_assert(std::is_same_v<IFE_FileMap, Abstraction::FileMap>);
static_assert(std::is_same_v<IFE_FileMapEntry, Abstraction::FileMapEntry>);
static_assert(std::is_same_v<IFE_Gap, Abstraction::Gap>);
static_assert(std::is_same_v<IFE_GapClass, Abstraction::GapClass>);
static_assert(std::is_same_v<IFE_ProducerFailure, Abstraction::ProducerFailure>);
static_assert(std::is_same_v<IFE_ProducerFailureKind, Abstraction::ProducerFailureKind>);
static_assert(std::is_same_v<IFE_BlockRef, Abstraction::BlockRef>);
static_assert(std::is_same_v<IFE_BlockVerdict, Abstraction::BlockVerdict>);
static_assert(std::is_same_v<IFE_RepairClass, Abstraction::RepairClass>);
static_assert(std::is_same_v<IFE_RecoveryReport, Abstraction::RecoveryReport>);

// The wire-tag <-> map-entry vocabulary (generated IFE_Map.hpp) is reachable
// from the advanced tier: the map is its consumer.
static_assert(std::is_same_v<decltype(Abstraction::entry_for(::Iris::File::constants::RecoveryCodes::RECOVER_TILE_TABLE)), Abstraction::MapEntryType>);
static_assert(std::is_same_v<decltype(Abstraction::recovery_for(Abstraction::MAP_ENTRY_TILE_TABLE)), ::Iris::File::constants::RecoveryCodes>);

// ---------------------------------------------------------------------------
// Availability: naming each member fails the build if it is removed.
// Instanced from main() so the body compiles.
// ---------------------------------------------------------------------------
void touch_advanced_members(Abstraction::FileMap&      map,
                            Abstraction::BlockVerdict& verdict,
                            Abstraction::BlockRef&     ref,
                            Abstraction::RecoveryReport& report) {
    (void)map.file_size;
    (void)map.gaps;
    (void)map.failures;
    (void)verdict.block;
    (void)verdict.class_;
    (void)verdict.bit_cost;
    (void)verdict.candidates;
    (void)verdict.repaired;
    (void)ref.parent;
    (void)ref.slot;
    (void)ref.expected;
    (void)ref.target;
    (void)ref.validation;
    (void)ref.recovery;
    (void)ref.extent;
    (void)report.blocks_total;
    (void)report.blocks;
    (void)report.holes;
    (void)report.gaps;
    (void)report.failures;
}

// ---------------------------------------------------------------------------
// Runtime smoke: a non-Iris buffer is handled without crashing. The producers
// may throw (an unreadable header) — the point is that they do not crash and
// do not invent entries.
// ---------------------------------------------------------------------------
void smoke() {
    BYTE buffer[256];
    std::memset(buffer, 0, sizeof buffer);
    const FileAccessInfo info{buffer, sizeof buffer};

    try {
        const Abstraction::FileMap scanned = recover_file_structure(info);
        // Zeroed bytes carry no block signature, so the census finds nothing —
        // and must NOT fabricate a block to fill the map.
        IFE_CHECK(scanned.empty());
    } catch (const std::exception&) {
        // Refusing to read a headerless buffer is a correct outcome.
    }

    try {
        (void)generate_file_map(info);
    } catch (const std::exception&) {
    }

    // A zero-length mapping is a legal "no bytes" query.
    const FileAccessInfo empty{nullptr, 0};
    try {
        IFE_CHECK(recover_file_structure(empty).empty());
    } catch (const std::exception&) {
    }
}

}  // namespace

int main() {
    Abstraction::FileMap map;
    Abstraction::BlockVerdict verdict;
    Abstraction::BlockRef ref;
    Abstraction::RecoveryReport report;
    touch_advanced_members(map, verdict, ref, report);
    smoke();

    if (g_failures == 0) std::printf("ife_advanced_api_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
