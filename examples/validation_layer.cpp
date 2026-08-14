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
 * Attaching is three lines: copy `conformance_layer()`, point its
 * `diagnostic` at a string you own, pass the hooks to store(). A failing
 * clause comes back twice — as the returned `Status` (code/block/field) and,
 * if a sink is set, as the formatted diagnostic citing the spec clause.
 * Layers compose: set `next` to chain a tracing layer in front.
 *
 * Build: the conformance layer lives in generated_source/IFE_Validation.{hpp,cpp}
 * (kept out of the library on purpose — see the header's comment), so this
 * example compiles it alongside the generated block layer, exactly as
 * tests/ife_validation_tests.cpp does.
 */
#include "IFE_Validation.hpp"

#include <cstdio>
#include <exception>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

using ::IFE::BYTE;

namespace b = ::IFE::blocks;
namespace k = ::IFE::constants;

namespace {

std::vector<BYTE> buffer() { return std::vector<BYTE>(4096, 0); }

/// Every expectation below fails loudly: a demonstration that silently
/// accepts the wrong behaviour teaches the wrong lesson. Throwing keeps the
/// failure in the message rather than in a return code the reader must
/// remember to check. `std::source_location` supplies file and line for free.
void expect(bool ok, const char* what,
            const std::source_location loc = std::source_location::current()) {
    if (!ok) throw std::runtime_error(std::string(what) + " (" + loc.file_name() +
                                 ":" + std::to_string(loc.line()) + ")");
}

// A layer extent set that is structurally perfect but violates the spec: a
// layer with zero tiles.
const std::vector<b::LayerExtentEntry> ZERO_TILES = {
    {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f},
    {.X_TILES = 0, .Y_TILES = 4, .SCALE = 2.0f},   // X_TILES shall be >= 1
};

}  // namespace

int main() {
    try {
        // ---- 1. detached: conformance is NOT enforced -------------------- //
        // Structurally valid, spec-violating. With no layer attached this
        // stores and reports success — conformance is not the library's
        // business unless an application asks for it.
        {
            auto f = buffer();
            const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES};
            expect(static_cast<bool>(b::store(f.data(), 0, bad)),
                   "a detached store accepts even spec-violating input");
            std::printf("1. detached store of zero-tile extents: accepted (one null check)\n");
        }

        // ---- 2. attached: a spec violation becomes a CONFORMANCE failure -- //
        // The pattern every case below uses: copy the shared layer, point its
        // sink at a string, hand the hooks to store().
        {
            auto f = buffer();
            std::string why;
            auto hooks = b::conformance_layer();
            hooks.diagnostic = &why;

            const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES};
            const auto status = b::store(f.data(), 0, bad, &hooks);
            expect(!status, "an attached store rejects spec-violating input");
            expect(status.code == b::Check::CONFORMANCE, "the rejection is a conformance failure");
            expect(why.find("X_TILES") != std::string::npos, "the diagnostic names the clause");
            std::printf("2. attached store of zero-tile extents: %s.%s\n      %s\n",
                        status.block, status.field, why.c_str());
        }

        // ---- 3. the ordering clause is a property of the sequence -------- //
        {
            auto f = buffer();
            std::string why;
            auto hooks = b::conformance_layer();
            hooks.diagnostic = &why;

            const std::vector<b::LayerExtentEntry> flat = {
                {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f},
                {.X_TILES = 4, .Y_TILES = 4, .SCALE = 2.0f},
                {.X_TILES = 8, .Y_TILES = 8, .SCALE = 2.0f},   // shall strictly increase
            };
            const b::LayerExtentsCreateInfo bad{.entries = flat};
            const auto status = b::store(f.data(), 0, bad, &hooks);
            expect(!status, "a flat scale fails the ordering clause");
            expect(why.find("SCALE") != std::string::npos, "the diagnostic names the field");
            std::printf("3. attached store of flat-scale extents: %s.%s\n      %s\n",
                        status.block, status.field, why.c_str());
        }

        // ---- 4. enum membership: in the width, not in the domain --------- //
        {
            auto f = buffer();
            std::string why;
            auto hooks = b::conformance_layer();
            hooks.diagnostic = &why;

            const b::TileTableCreateInfo bad{
                .ENCODING = static_cast<k::TileEncodings>(200),
                .FORMAT   = k::PixelFormats::FORMAT_R8G8B8A8,
            };
            const auto status = b::store(f.data(), 0, bad, &hooks);
            expect(!status, "an undeclared encoding fails enum membership");
            expect(why.find("TILE_TABLE.ENCODING") != std::string::npos,
                   "the diagnostic names the field");
            std::printf("4. attached store of tile-encoding 200: %s.%s\n      %s\n",
                        status.block, status.field, why.c_str());
        }

        // ---- 5. mandatory pointers: a file header that points nowhere ---- //
        {
            auto f = buffer();
            std::string why;
            auto hooks = b::conformance_layer();
            hooks.diagnostic = &why;

            const b::FileHeaderCreateInfo bad{
                .TILE_TABLE_OFFSET = ::IFE::constants::NULL_OFFSET,
                .METADATA_OFFSET   = 128,
            };
            const auto status = b::store(f.data(), 0, bad, &hooks);
            expect(!status, "a NULL mandatory pointer fails the clause");
            expect(why.find("TILE_TABLE_OFFSET") != std::string::npos,
                   "the diagnostic names the field");
            std::printf("5. attached store of NULL tile-table pointer: %s.%s\n      %s\n",
                        status.block, status.field, why.c_str());
        }

        // ---- 6. conformant input passes with the layer attached ---------- //
        {
            auto f = buffer();
            std::string why;
            auto hooks = b::conformance_layer();
            hooks.diagnostic = &why;

            const std::vector<b::LayerExtentEntry> good = {
                {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f},
                {.X_TILES = 4, .Y_TILES = 4, .SCALE = 2.0f},
                {.X_TILES = 8, .Y_TILES = 8, .SCALE = 4.0f},
            };
            const b::LayerExtentsCreateInfo ok{.entries = good};
            expect(static_cast<bool>(b::store(f.data(), 0, ok, &hooks)),
                   "a conformant store passes with the layer attached");
            expect(why.empty(), "nothing to report for a conformant input");
            std::printf("6. attached store of conformant extents: accepted, no diagnostic\n");
        }

        // ---- 7. layering: a tracing layer composes via `next` ------------- //
        {
            auto f = buffer();
            std::string why;
            auto conformance = b::conformance_layer();
            conformance.diagnostic = &why;

            b::ValidationHooks tracing{};
            tracing.LAYER_EXTENTS = [](const b::LayerExtentsCreateInfo& info, ::IFE::Offset at,
                                       const b::ValidationHooks* self) -> b::Status {
                std::printf("    [trace] LAYER_EXTENTS at %llu, %zu entries\n",
                            static_cast<unsigned long long>(at), info.entries.size());
                // Forward down the chain; the conformance layer runs next and
                // writes through the sink attached to `conformance`.
                if (self->next != nullptr && self->next->LAYER_EXTENTS != nullptr)
                    return self->next->LAYER_EXTENTS(info, at, self->next);
                return {};
            };
            tracing.next = &conformance;

            const b::LayerExtentsCreateInfo bad{.entries = ZERO_TILES};
            const auto status = b::store(f.data(), 0, bad, &tracing);
            expect(!status, "the chain still rejects spec-violating input");
            expect(status.code == b::Check::CONFORMANCE, "the rejection is a conformance failure");
            expect(!why.empty(), "the conformance layer wrote through the shared sink");
            std::printf("7. layered store of zero-tile extents: traced, then %s.%s\n",
                        status.block, status.field);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "validation_layer: FAILED — %s\n", e.what());
        return 1;
    }

    std::printf("validation_layer: all checks passed\n");
    return 0;
}
