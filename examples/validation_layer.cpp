/**
 * @file validation_layer.cpp
 * @brief The spec-conformance layer, attached and detached — a worked example.
 *
 * Validation in IFE is split in two, and the split is the thing worth
 * understanding before writing an encoder:
 *
 *   1. **Structural** validation is inline and mandatory. A block stores its
 *      own offset, carries its recovery tag, and fits in the file; every
 *      store() checks that, always, whether or not a layer is attached.
 *   2. **Conformance** validation is optional and attachable. `X_TILES >= 1`,
 *      strictly increasing layer scale, enum membership: real requirements of
 *      the specification that say nothing about whether the bytes are
 *      readable. The generated `check_*` functions implement them, and a
 *      store() consults them only when given a `ValidationHooks` chain.
 *
 * That is the Vulkan validation-layer arrangement, and it is deliberate:
 * conformance policy is a development tool, not a production dependency. A
 * shipped product links the layer only when it wants it; the default store()
 * costs one null check.
 *
 * This example walks both paths with real inputs — a spec-violating layer
 * extent set, a bad tile-encoding enum, a file header that points nowhere —
 * and shows what each failure looks like through the layer's diagnostic
 * callback. It also chains a tracing layer on top of the conformance layer,
 * which is how a future tool composes rather than competes.
 *
 * Build: the conformance layer lives in generated_source/IFE_Validation.{hpp,cpp}
 * (kept out of the library on purpose — see the header's comment), so this
 * example compiles it alongside the generated block layer, exactly as
 * tests/ife_validation_tests.cpp does.
 */
#include "IFE_Validation.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using ::IFE::BYTE;

namespace b = ::IFE::blocks;
namespace k = ::IFE::constants;

namespace {

int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

/// Where the layer's diagnostics go. The layer formats; the caller decides
/// what I/O means — here, a counter and a captured message.
std::string g_diagnostic;

void collect(const char* __message, void* /*__user*/) {
    g_diagnostic = __message;
}

/// A copy of the shared conformance layer with diagnostics attached. The
/// returned struct is immutable and shared; copy it before touching anything.
b::ValidationHooks attached() {
    b::ValidationHooks hooks = b::conformance_layer();
    hooks.diagnostic = &collect;
    return hooks;
}

// A layer extent set that is structurally perfect and violates the spec: a
// layer with zero tiles, and a scale that does not strictly increase.
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

}  // namespace

int main() {
    // ---- 1. detached: conformance is NOT enforced ------------------------ //
    // Structurally valid, spec-violating. With no layer attached this stores
    // and reports success — which is the point: conformance is not the
    // library's business unless an application asks for it. The block really
    // is readable; the violation is semantic, not structural.
    {
        auto f = buffer();
        const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES, .count = 2};
        const auto status = b::store(f.data(), 0, bad);
        CHECK(static_cast<bool>(status));
        std::printf("detached store of zero-tile extents: %s (one null check)\n",
                    static_cast<bool>(status) ? "accepted" : "rejected");
    }

    // ---- 2. attached: the same input is now a CONFORMANCE failure --------- //
    {
        auto f = buffer();
        auto hooks = attached();
        g_diagnostic.clear();

        const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES, .count = 2};
        const auto status = b::store(f.data(), 0, bad, &hooks);

        CHECK(!status);
        CHECK(status.code == b::Check::CONFORMANCE);
        CHECK(g_diagnostic.find("X_TILES") != std::string::npos);
        std::printf("attached store of zero-tile extents: %s.%s — %s\n",
                    status.block, status.field, g_diagnostic.c_str());
    }

    // ---- 3. the ordering clause is a property of the sequence ------------ //
    {
        auto f = buffer();
        auto hooks = attached();
        g_diagnostic.clear();

        const b::LayerExtentsCreateInfo bad{.entries = FLAT_SCALE, .count = 3};
        const auto status = b::store(f.data(), 0, bad, &hooks);

        CHECK(!status);
        CHECK(status.code == b::Check::CONFORMANCE);
        CHECK(g_diagnostic.find("SCALE") != std::string::npos);
        std::printf("attached store of flat-scale extents: %s.%s — %s\n",
                    status.block, status.field, g_diagnostic.c_str());
    }

    // ---- 4. enum membership: a value in the width but not the domain ----- //
    {
        auto f = buffer();
        auto hooks = attached();
        g_diagnostic.clear();

        b::TileTableCreateInfo table{};
        table.ENCODING = static_cast<k::TileEncodings>(200);
        table.FORMAT   = k::PixelFormats::FORMAT_R8G8B8A8;

        const auto status = b::store(f.data(), 0, table, &hooks);

        CHECK(!status);
        CHECK(status.code == b::Check::CONFORMANCE);
        CHECK(status.found == 200);
        CHECK(g_diagnostic.find("TILE_TABLE.ENCODING") != std::string::npos);
        std::printf("attached store of tile-encoding 200: %s.%s — %s\n",
                    status.block, status.field, g_diagnostic.c_str());
    }

    // ---- 5. mandatory pointers: a file header that points nowhere -------- //
    {
        auto f = buffer();
        auto hooks = attached();
        g_diagnostic.clear();

        b::FileHeaderCreateInfo header{};
        header.TILE_TABLE_OFFSET = ::IFE::constants::NULL_OFFSET;
        header.METADATA_OFFSET   = 128;

        const auto status = b::store(f.data(), 0, header, &hooks);

        CHECK(!status);
        CHECK(status.code == b::Check::CONFORMANCE);
        CHECK(g_diagnostic.find("TILE_TABLE_OFFSET") != std::string::npos);
        std::printf("attached store of NULL tile-table pointer: %s.%s — %s\n",
                    status.block, status.field, g_diagnostic.c_str());
    }

    // ---- 6. conformant input passes with the layer attached -------------- //
    {
        auto f = buffer();
        auto hooks = attached();
        g_diagnostic.clear();

        const b::LayerExtentsCreateInfo good{.entries = GOOD, .count = 3};
        const auto status = b::store(f.data(), 0, good, &hooks);

        CHECK(static_cast<bool>(status));
        CHECK(g_diagnostic.empty());
        std::printf("attached store of conformant extents: accepted, no diagnostics\n");
    }

    // ---- 7. layering: a tracing layer composes via `next` ---------------- //
    {
        auto f = buffer();
        auto conformance = attached();          // the real layer, with diagnostics
        b::ValidationHooks tracing{};           // a fresh chain head
        tracing.LAYER_EXTENTS = [](const b::LayerExtentsCreateInfo& __info,
                                   ::IFE::Offset __at,
                                   const b::ValidationHooks* __self) -> b::Status {
            std::printf("    [trace] LAYER_EXTENTS at %llu, %u entries\n",
                        static_cast<unsigned long long>(__at), __info.count);
            // Forward down the chain; the conformance layer runs next.
            if (__self->next != nullptr && __self->next->LAYER_EXTENTS != nullptr)
                return __self->next->LAYER_EXTENTS(__info, __at, __self->next);
            return {};
        };
        tracing.next = &conformance;
        g_diagnostic.clear();

        const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES, .count = 2};
        const auto status = b::store(f.data(), 0, bad, &tracing);

        CHECK(!status);
        CHECK(status.code == b::Check::CONFORMANCE);
        CHECK(!g_diagnostic.empty());
        std::printf("layered store of zero-tile extents: traced, then %s.%s — %s\n",
                    status.block, status.field, g_diagnostic.c_str());
    }

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("validation_layer: all checks passed\n");
    return 0;
}
