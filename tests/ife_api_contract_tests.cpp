/**
 * @file ife_api_contract_tests.cpp
 * @brief Phase 0 of the FastFHIR → IFE migration (fastfhir_migration_handoff.md):
 *        freeze the exact IFE surface Iris-Codec consumes, so a migration edit
 *        fails HERE before it reaches the codec's own build.
 *
 * WHY THIS EXISTS. Iris-Codec (`/Users/ryanlandvater/GitHub/Iris-Codec`) pulls
 * IFE with `FetchContent(GIT_TAG "origin/main")` and links the objects with
 * `IFE_EXPORT=`. There is no version pin between them: a rename or a removed
 * member on IFE `main` breaks every codec build on its next configure. This
 * test names each symbol and member the codec touches — the compile IS the
 * assertion, and it runs in IFE's own CI before anything is pushed.
 *
 * SELF-CONTAINED (no external framework). Non-zero exit on failure.
 */
#include "IrisFileExtension.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <type_traits>
#include <utility>

// Two directives, not one: Iris::File no longer re-exports the core with a
// `using namespace Iris;` of its own, so a consumer that wants both layers
// unqualified names both — the dependency is written where it is used.
using namespace Iris;
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
// Compile-time contract: the exact types Iris-Codec names.
// Sites: IrisCodecSlide.cpp:37,62,216; IrisCodecEncoder.cpp:702,848,889,1323;
//        IrisCodecDeriveLayers.cpp:93; IrisCodecPrivTypes.hpp:144.
// ---------------------------------------------------------------------------

// The one mapped-file argument every file-level entry point takes.
static_assert(std::is_pointer_v<decltype(FileAccessInfo::file_ptr)>);
static_assert(std::is_integral_v<decltype(FileAccessInfo::file_size)>);

// The three entry points the codec calls. The file-map PRODUCER
// (generate_file_map) is the ADVANCED tier now — its contract is frozen in
// ife_advanced_api_tests.cpp.
using IfeResult = decltype(is_iris_codec_file(std::declval<const FileAccessInfo&>()));
static_assert(std::is_same_v<IfeResult, decltype(validate_file_structure(std::declval<const FileAccessInfo&>()))>);
static_assert(std::is_same_v<IfeResult, Iris::Result>);  // the codec's `Iris::Result`
static_assert(std::is_same_v<decltype(abstract_file_structure(std::declval<const FileAccessInfo&>())), Abstraction::File>);

// The abstraction members the codec reads straight through.
static_assert(std::is_same_v<decltype(Abstraction::File::header), Abstraction::Header>);
static_assert(std::is_same_v<decltype(Abstraction::File::tileTable), Abstraction::TileTable>);
static_assert(std::is_same_v<decltype(Abstraction::File::images), Abstraction::AssociatedImages>);

// Phase 1: the READ tier's global aliases mirror the namespaced types
// one-for-one. A rename on either side of the alias fails here. The ADVANCED
// tier's aliases (IFE_FileMap, IFE_BlockRef, ...) are asserted in
// ife_advanced_api_tests.cpp.
static_assert(std::is_same_v<IFE_FileAccessInfo, FileAccessInfo>);
static_assert(std::is_same_v<IFE_File, Abstraction::File>);
static_assert(std::is_same_v<IFE_TileTable, Abstraction::TileTable>);
static_assert(std::is_same_v<IFE_TileEntry, Abstraction::TileEntry>);

// ---------------------------------------------------------------------------
// Availability contract: naming each consumed member fails the build if it is
// removed or renamed. Instanced from main(), so the body is compiled.
// ---------------------------------------------------------------------------
void touch_frozen_members(Abstraction::File& file, Abstraction::TileTable& table) {
    // Abstraction::File — IrisCodecSlide.cpp:227-340.
    (void)file.header;
    (void)file.tileTable;
    (void)file.images;
    (void)file.metadata;        // -> .codec  (IrisCodecSlide.cpp:227)
    (void)file.annotations;
    (void)file.attributeTree;
    (void)file.clinicalOffset;
    (void)file.clinicalSize;
    // Abstraction::TileTable — IrisCodecSlide.cpp:232-235, IrisCodecEncoder.cpp.
    (void)table.encoding;
    (void)table.format;
    (void)table.extent;
    (void)table.layers;
    (void)table.tileLength;
    // The exact chains the codec follows.
    (void)table.layers;
    (void)file.metadata.codec;  // IrisCodecSlide.cpp:227
}

// ---------------------------------------------------------------------------
// Runtime smoke: a non-Iris buffer is refused, never crashes.
// ---------------------------------------------------------------------------
void smoke() {
    BYTE buffer[64];
    std::memset(buffer, 0, sizeof buffer);
    const FileAccessInfo info{buffer, sizeof buffer};

    // The two noexcept predicates answer "not an Iris file" rather than crash.
    IFE_CHECK(is_iris_codec_file(info) != IRIS_SUCCESS);
    IFE_CHECK(validate_file_structure(info) != IRIS_SUCCESS);

    // The read path refuses a damaged file by throwing, per its contract.
    bool threw = false;
    try {
        (void)abstract_file_structure(info);
    } catch (const std::exception&) {
        threw = true;
    }
    IFE_CHECK(threw);

    // A zero-length mapping is a legal "no bytes" query.
    const FileAccessInfo empty{nullptr, 0};
    IFE_CHECK(is_iris_codec_file(empty) != IRIS_SUCCESS);
}

}  // namespace

int main() {
    Abstraction::File file;
    Abstraction::TileTable table;
    touch_frozen_members(file, table);
    smoke();

    if (g_failures == 0) std::printf("ife_api_contract_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
