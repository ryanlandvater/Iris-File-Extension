/**
 * @file ife_builder_tests.cpp
 * @brief The IFE write handle (`Iris::File::Builder`): build a slide with it,
 *        read it back with the public read path, assert the round trip.
 *
 * What this pins:
 *   - `Builder::create` maps an arena; the block tier (`builder->append`)
 *     claims + stores a block and returns its offset; `builder->seal` writes
 *     the FILE_HEADER and settles the backing file's length. (The
 *     application tier — set_tile_table / append_tile / finalize — is
 *     ife_builder_layered_tests.)
 *   - The bytes the builder produces pass `is_iris_codec_file`,
 *     `validate_file_structure`, and `abstract_file_structure`.
 *   - `claim` refuses to write past capacity (the deferred-expansion contract).
 *   - The FILE_HEADER region is the builder's: no claim ever lands in it, so
 *     `finalize` writing the header cannot overwrite a tile (the 2026-09-28
 *     defect: the head started at 0 and the first two tiles were destroyed
 *     while validation still passed).
 *
 * Self-contained (no framework). Non-zero exit on failure.
 */
#include "IFE_Builder.hpp"          // the body + the templated append
#include "IFE_Parser.hpp"           // the read body + handle forwarders
#include "IrisFileExtension.hpp"    // the public read path

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace b = ::Iris::File::blocks;
namespace k = ::Iris::File::constants;

using ::Iris::File::BYTE;
using ::Iris::File::Offset;
using Iris::IRIS_SUCCESS;

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

constexpr std::uint32_t TILE_BYTES = 16;

/// Build a one-layer 2x2 slide through the Builder and return it sealed.
/// Each tile's bytes are filled with its own value (0xC0 + index) so the
/// caller can prove every byte survived `finalize`. `tamper`, when given, edits
/// the tile entries after they are claimed and before they are written — the
/// way a test manufactures a file whose entries lie.
Iris::File::Builder build_minimal_slide(
        std::vector<b::TileOffsetEntry>& tiles,
        const std::function<void(std::vector<b::TileOffsetEntry>&)>& tamper = {},
        const Iris::File::BuilderCreateInfo& create = {.capacity = 1ULL << 20}) {  // 1 MiB anonymous
    Iris::File::Builder builder = Iris::File::Builder::create(create);

    // Four tiles: raw regions the tile-offsets entries address. Claim them
    // first so the entries can carry real, in-file offsets.
    tiles.assign(4, b::TileOffsetEntry{});
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const Offset at = builder->claim(TILE_BYTES);
        tiles[i].OFFSET = at;
        tiles[i].SIZE   = TILE_BYTES;
        std::memset(builder->base() + at, static_cast<int>(0xC0 + i), TILE_BYTES);
    }
    if (tamper) tamper(tiles);

    const std::vector<b::LayerExtentEntry> extents = {
        {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f}};

    // Leaves first, then the blocks that name them, then the header — order on
    // disk does not matter (offsets are absolute), but an offset must exist
    // before it is written.
    const Offset extents_at = builder->append(b::LayerExtentsCreateInfo{.entries = extents});
    const Offset tiles_at   = builder->append(b::TileOffsetsCreateInfo{.entries = tiles});
    const Offset table_at   = builder->append(b::TileTableCreateInfo{
        .ENCODING             = k::TileEncodings::TILE_ENCODING_JPEG,
        .FORMAT               = k::PixelFormats::FORMAT_R8G8B8A8,
        .TILE_OFFSETS_OFFSET  = tiles_at,
        .LAYER_EXTENTS_OFFSET = extents_at,
        .X_EXTENT             = 512,
        .Y_EXTENT             = 512});
    const Offset meta_at    = builder->append(b::MetadataCreateInfo{.MICRONS_PIXEL = 0.25f});

    builder->seal(b::FileHeaderCreateInfo{
        .TILE_TABLE_OFFSET = table_at,
        .METADATA_OFFSET   = meta_at});

    return builder;
}

/// How one tile of a Z-stacked test slide is framed.
struct FrameSpec {
    bool          framed     = true;
    std::uint32_t tile_index = 0;   ///< what the frame claims
    std::uint16_t z_planes   = 0;   ///< what the frame claims
};

/// A one-layer 2x2 slide whose layer declares `layer_planes`, built with the
/// block tier so a frame can be omitted or made to lie — which the Builder's
/// application tier refuses to do.
Iris::File::Builder build_framed_slide(std::uint16_t layer_planes,
                                       const std::vector<FrameSpec>& frames) {
    Iris::File::Builder builder = Iris::File::Builder::create({.capacity = 1ULL << 20});
    std::vector<b::TileOffsetEntry> tiles(4);
    for (std::size_t i = 0; i < 4; ++i) {
        Offset anchor = 0;
        if (frames[i].framed) {
            const Offset at = builder->claim(b::TILE_PIXEL_DATA::header_size + TILE_BYTES);
            anchor = at + b::TILE_PIXEL_DATA::header_size;
            (void)b::store(builder->base(), anchor, b::TilePixelDataCreateInfo{
                .TILE_INDEX = frames[i].tile_index, .Z_PLANES = frames[i].z_planes});
        } else {
            anchor = builder->claim(TILE_BYTES);
        }
        std::memset(builder->base() + anchor, 0xEE, TILE_BYTES);
        tiles[i] = {.OFFSET = anchor, .SIZE = TILE_BYTES};
    }
    const std::vector<b::LayerExtentEntry> extents = {
        {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f, .Z_PLANES = layer_planes}};
    const Offset extents_at = builder->append(b::LayerExtentsCreateInfo{.entries = extents});
    const Offset tiles_at   = builder->append(b::TileOffsetsCreateInfo{.entries = tiles});
    const Offset table_at   = builder->append(b::TileTableCreateInfo{
        .ENCODING = k::TileEncodings::TILE_ENCODING_JPEG,
        .FORMAT = k::PixelFormats::FORMAT_R8G8B8A8,
        .TILE_OFFSETS_OFFSET = tiles_at, .LAYER_EXTENTS_OFFSET = extents_at,
        .X_EXTENT = 512, .Y_EXTENT = 512});
    const Offset meta_at = builder->append(b::MetadataCreateInfo{});
    builder->seal(b::FileHeaderCreateInfo{.TILE_TABLE_OFFSET = table_at,
                                          .METADATA_OFFSET = meta_at});
    return builder;
}

}  // namespace

int main() {
    // ---- 1. Build ---------------------------------------------------------- //
    std::vector<b::TileOffsetEntry> tiles;
    Iris::File::Builder builder = build_minimal_slide(tiles);
    IFE_CHECK(static_cast<bool>(builder));
    IFE_CHECK(builder.head() > 0);

    // ---- 1b. The header region is reserved: every tile survived finalize --- //
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        IFE_CHECK(tiles[i].OFFSET >= b::FILE_HEADER::header_size);
        std::size_t intact = 0;
        for (std::uint32_t j = 0; j < TILE_BYTES; ++j)
            intact += builder->base()[tiles[i].OFFSET + j] == static_cast<BYTE>(0xC0 + i);
        IFE_CHECK(intact == TILE_BYTES);
    }

    const Iris::File::FileAccessInfo info{
        reinterpret_cast<const BYTE*>(builder->base()),
        builder.head()};

    // ---- 2. The structural gates ------------------------------------------ //
    IFE_CHECK(Iris::File::is_iris_codec_file(info) == IRIS_SUCCESS);
    IFE_CHECK(Iris::File::validate_file_structure(info) == IRIS_SUCCESS);

    // ---- 3. The abstraction round-trips ----------------------------------- //
    const Iris::File::Abstraction::File file = Iris::File::abstract_file_structure(info);
    IFE_CHECK(file.header.fileSize == builder.head());
    IFE_CHECK(file.tileTable.extent.width == 512);
    IFE_CHECK(file.tileTable.extent.height == 512);
    IFE_CHECK(file.tileTable.extent.layers.size() == 1);
    if (!file.tileTable.extent.layers.empty()) {
        IFE_CHECK(file.tileTable.extent.layers[0].xTiles == 2);
        IFE_CHECK(file.tileTable.extent.layers[0].yTiles == 2);
    }
    IFE_CHECK(file.tileTable.layers.size() == 1);
    if (!file.tileTable.layers.empty()) {
        IFE_CHECK(file.tileTable.layers[0].size() == 4);
        // Every tile entry survived with its offset and size.
        for (const auto& entry : file.tileTable.layers[0]) {
            IFE_CHECK(entry.offset != k::NULL_OFFSET);
            IFE_CHECK(entry.size == TILE_BYTES);
        }
    }
    IFE_CHECK(file.metadata.micronsPerPixel == 0.25f);

    // ---- 4. The same bytes through the READ handle ------------------------ //
    {
        const Iris::File::Parser parser(info);
        IFE_CHECK(static_cast<bool>(parser));
        IFE_CHECK(parser.is_iris_codec_file() == IRIS_SUCCESS);
        IFE_CHECK(parser.validate_file_structure() == IRIS_SUCCESS);
        const auto via_parser = parser.abstract_file_structure();
        IFE_CHECK(via_parser.tileTable.extent.width == 512);
        IFE_CHECK(parser.data() == reinterpret_cast<const BYTE*>(builder->base()));
        IFE_CHECK(parser.size() == builder.head());
    }

    // ---- 5. Capacity is a hard bound (never remapped) ----------------------- //
    {
        const auto tiny = Iris::File::Builder::create(
            Iris::File::BuilderCreateInfo{.capacity = 64});
        bool threw = false;
        try {
            // One block far larger than the 64-byte arena.
            (void)tiny->claim(4096);
        } catch (const std::exception&) {
            threw = true;
        }
        IFE_CHECK(threw);
    }

    // ---- 6. Lock-free claim: concurrent producers get distinct ranges ------ //
    {
        const auto shared = Iris::File::Builder::create(
            Iris::File::BuilderCreateInfo{.capacity = 4ULL * 1024 * 1024});

        constexpr int THREADS = 8, CLAIMS_PER_THREAD = 500, CHUNK = 16;
        constexpr Offset HEADER = b::FILE_HEADER::header_size;
        std::mutex mtx;
        std::set<Offset> seen;
        bool overlap = false;

        std::vector<std::thread> pool;
        pool.reserve(THREADS);
        for (int t = 0; t < THREADS; ++t) {
            pool.emplace_back([&] {
                for (int i = 0; i < CLAIMS_PER_THREAD; ++i) {
                    const Offset at = shared->claim(CHUNK);
                    std::lock_guard<std::mutex> lock(mtx);
                    // Every chunk must be CHUNK-aligned relative to the first
                    // claimable byte (the header region comes first) and wholly
                    // distinct.
                    if (at < HEADER || (at - HEADER) % CHUNK != 0 ||
                        !seen.insert(at).second ||
                        !seen.insert(at + CHUNK - 1).second)
                        overlap = true;
                }
            });
        }
        for (auto& th : pool) th.join();

        IFE_CHECK(!overlap);
        IFE_CHECK(shared->head() ==
                  HEADER + static_cast<Offset>(THREADS) * CLAIMS_PER_THREAD * CHUNK);
    }

    // ---- 7. A tile entry is a claim: the read path checks it ---------------- //
    // The generated walk cannot follow a tile entry (a stream has no header),
    // so validate_file_structure and abstract_file_structure check each one:
    // it may not start inside the FILE_HEADER or run past the end of the file.
    // NULL_TILE is "no tile here" and is always legal.
    {
        struct Case {
            const char* name;
            std::function<void(std::vector<b::TileOffsetEntry>&)> tamper;
            bool valid;
        };
        const Case cases[] = {
            {"a tile inside the file header",
             [](auto& t) { t[3].OFFSET = 0; }, false},
            {"a tile running past the end of the file",
             [](auto& t) { t[2].SIZE = 0xFFFFFF; }, false},
            {"a tile starting past the end of the file",
             [](auto& t) { t[1].OFFSET = 0xFFFFFFFF; }, false},
            {"a NULL_TILE entry (no tile at that grid position)",
             [](auto& t) { t[0].OFFSET = k::NULL_TILE; t[0].SIZE = 0; }, true},
            {"a NULL_TILE entry with a nonzero SIZE",
             [](auto& t) { t[0].OFFSET = k::NULL_TILE; }, true},
        };
        for (const Case& c : cases) {
            std::vector<b::TileOffsetEntry> t;
            const Iris::File::Builder built = build_minimal_slide(t, c.tamper);
            const Iris::File::FileAccessInfo at{built->base(), built.head()};
            const bool validated = Iris::File::validate_file_structure(at) == IRIS_SUCCESS;
            bool abstracted = true;
            try { (void)Iris::File::abstract_file_structure(at); }
            catch (const std::exception&) { abstracted = false; }
            if (validated != c.valid || abstracted != c.valid)
                std::fprintf(stderr, "  case: %s (validate %d, abstract %d, expected %d)\n",
                             c.name, validated, abstracted, c.valid);
            IFE_CHECK(validated == c.valid);
            IFE_CHECK(abstracted == c.valid);
        }
    }

    // ---- 8. A huge anonymous reservation costs nothing up front ------------ //
    // Arenas are never remapped, so they are reserved large. An anonymous one
    // must not be charged against memory for the whole range: Iris::Memory maps
    // it MAP_NORESERVE (Linux's default overcommit refuses a mapping larger than
    // RAM + swap otherwise) and SEC_RESERVE on Windows (which would otherwise
    // commit it all against the pagefile), committing each claim before it is
    // touched. 1 TiB here is the check on the CI runners this Mac cannot be;
    // the claims stay small, since on Windows a claim commits what it takes.
    {
        std::vector<b::TileOffsetEntry> t;
        bool built = true;
        try {
            const Iris::File::Builder huge = build_minimal_slide(
                t, {}, Iris::File::BuilderCreateInfo{.capacity = Iris::File::Size{1} << 40});
            const Iris::File::FileAccessInfo at{huge->base(), huge.head()};
            IFE_CHECK(huge.capacity() == (Iris::File::Size{1} << 40));
            IFE_CHECK(Iris::File::validate_file_structure(at) == IRIS_SUCCESS);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  1 TiB anonymous builder: %s\n", e.what());
            built = false;
        }
        IFE_CHECK(built);
    }

    // ---- 9. finalize leaves a closed file the caller can move -------------- //
    // Choosing and moving files is the application's job (an encoder writes to
    // the temp directory and moves the finished file into place). The builder's
    // side of that contract: at finalize the file is complete, truncated to its
    // written size, and CLOSED — Windows can neither truncate nor move a file
    // that is still mapped, so this is the check that matters on that runner.
    {
        namespace fs = std::filesystem;
        std::random_device entropy;
        const std::string tag = std::to_string(entropy()) + std::to_string(entropy());
        const fs::path written = fs::temp_directory_path() / ("ife_builder_" + tag + ".iris");
        const fs::path moved   = fs::temp_directory_path() / ("ife_builder_" + tag + "_moved.iris");

        std::vector<b::TileOffsetEntry> t;
        Iris::File::Builder builder = build_minimal_slide(
            t, {}, Iris::File::BuilderCreateInfo{.filepath = written});   // default 8 GiB
        IFE_CHECK(builder.capacity() == (Iris::File::Size{8} << 30));
        IFE_CHECK(builder->sealed());
        IFE_CHECK(builder->base() == nullptr);                 // mapping released
        std::error_code ec;
        IFE_CHECK(fs::file_size(written, ec) == builder.head());

        // Move it while the builder handle is still alive, as an encoder would.
        fs::rename(written, moved, ec);
        IFE_CHECK(!ec);
        if (ec) std::fprintf(stderr, "  move after finalize: %s\n", ec.message().c_str());

        // No more writes once finalized.
        bool refused = false;
        try { (void)builder->claim(16); } catch (const std::logic_error&) { refused = true; }
        IFE_CHECK(refused);

        if (!ec) {
            Iris::Memory mapped;
            const Iris::Result opened = Iris::create_memory(
                Iris::MemoryCreateInfo{.filepath = moved, .read_only = true}, mapped);
            IFE_CHECK(static_cast<bool>(opened));
            if (mapped) {
                const Iris::File::FileAccessInfo at{mapped.base(), fs::file_size(moved)};
                IFE_CHECK(Iris::File::validate_file_structure(at) == IRIS_SUCCESS);
                for (std::size_t i = 0; i < t.size(); ++i)
                    IFE_CHECK(mapped.base()[t[i].OFFSET] == static_cast<BYTE>(0xC0 + i));
                mapped.close();
            }
        }
        fs::remove(written, ec);
        fs::remove(moved, ec);
    }

    // ---- 10. A Z-stacked layer frames every stream (the spec, 1.1) --------- //
    // The frame is the only place the format records a tile's plane count, so
    // on a layer whose Z_PLANES exceeds one every stream carries one, naming its
    // own tile and no more planes than the layer holds. validate and abstract
    // both enforce it; a single-plane layer may leave frames out.
    {
        const auto good = [](std::uint32_t i) { return FrameSpec{true, i, 3}; };
        struct Case { const char* name; std::uint16_t planes; std::vector<FrameSpec> frames; bool valid; };
        const Case cases[] = {
            {"every stream framed", 3, {good(0), good(1), good(2), good(3)}, true},
            {"one stream unframed", 3, {good(0), good(1), {false}, good(3)}, false},
            {"a frame naming another tile", 3, {good(0), {true, 3, 3}, good(2), good(3)}, false},
            {"a frame claiming more planes than the layer", 3, {good(0), good(1), {true, 2, 4}, good(3)}, false},
            {"a single-plane layer, one stream unframed", 0, {good(0), good(1), {false}, good(3)}, true},
        };
        for (const Case& c : cases) {
            const Iris::File::Builder built = build_framed_slide(c.planes, c.frames);
            const Iris::File::FileAccessInfo at{built->base(), built.head()};
            const bool validated = Iris::File::validate_file_structure(at) == IRIS_SUCCESS;
            bool abstracted = true;
            try { (void)Iris::File::abstract_file_structure(at); }
            catch (const std::exception&) { abstracted = false; }
            if (validated != c.valid || abstracted != c.valid)
                std::fprintf(stderr, "  case: %s (validate %d, abstract %d, expected %d)\n",
                             c.name, validated, abstracted, c.valid);
            IFE_CHECK(validated == c.valid);
            IFE_CHECK(abstracted == c.valid);
        }
    }

    if (g_failures == 0) std::printf("ife_builder_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
