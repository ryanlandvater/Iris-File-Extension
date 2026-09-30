/**
 * @file ife_builder_layered_tests.cpp
 * @brief The application tier of `Iris::File::Builder`: declare a tile table,
 *        append tiles (from many threads, in any order) and images, place the
 *        structure, finalize — and read back exactly what was put in.
 *
 * What this pins:
 *   - Round trip: every tile's bytes, the extent, one associated image (its
 *     orientation included), and the metadata come back through
 *     `abstract_file_structure` unchanged, and the file validates.
 *   - Frames: every framed stream carries a frame at the offset its tile
 *     entry names (the ANCHOR — CLAUDE.md), naming the right global tile index;
 *     recovery finds every reference intact.
 *   - NULL_TILE is a legitimate "no tile here"; an unaccounted tile is refused.
 *   - A Z-stacked layer frames every stream even with frames switched off.
 *   - Every misuse throws, and a refused `tile_offsets` or `finalize` writes
 *     nothing.
 *   - The layout is the caller's: the same inputs placed in two orders give
 *     two valid files that read back alike, each block where it was put — a
 *     METADATA block `claim`ed early and `fill`ed last included.
 *   - File-backed: `finalize` leaves a complete, closed file the caller moves.
 *
 * Self-contained (no framework). Non-zero exit on failure.
 */
#include "IFE_Builder.hpp"
#include "IrisFileExtension.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Iris;
using namespace Iris::File;

namespace b = ::Iris::File::blocks;
namespace k = ::Iris::File::constants;

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

/// Expect `fn` to throw exactly `E`.
template <class E>
bool throws(const std::function<void()>& fn) {
    try { fn(); } catch (const E&) { return true; } catch (...) { return false; }
    return false;
}

/// Two layers: layer 0 one tile, layer 1 a 2x2 grid — five global indices.
BuilderTileTableInfo two_layers(std::vector<uint16_t> planes = {}) {
    BuilderTileTableInfo info;
    info.encoding     = TILE_ENCODING_JPEG;
    info.format       = FORMAT_R8G8B8A8;
    info.extent.width  = 512;
    info.extent.height = 512;
    info.extent.layers = {LayerExtent{.xTiles = 1, .yTiles = 1, .scale = 0.5f},
                          LayerExtent{.xTiles = 2, .yTiles = 2, .scale = 1.0f}};
    info.planes = std::move(planes);
    return info;
}

/// Global index → (layer, tile) for two_layers().
struct Coord { uint32_t layer, tile; };
constexpr Coord COORDS[5] = {{0, 0}, {1, 0}, {1, 1}, {1, 2}, {1, 3}};

/// A distinct stream per global index: its own length and its own bytes.
std::vector<BYTE> stream_for(uint32_t global) {
    std::vector<BYTE> s(20 + 7 * global);
    for (size_t j = 0; j < s.size(); ++j) s[j] = static_cast<BYTE>(global * 37 + j);
    return s;
}

Metadata some_metadata() {
    Metadata metadata;
    metadata.codec = {2025, 3, 2};
    metadata.attributes.type    = METADATA_I2S;
    metadata.attributes.version = 1;
    metadata.attributes["b.key"] = u8"two";
    metadata.attributes["a.key"] = u8"one";
    metadata.ICC_profile.assign(100, '\0');
    for (size_t i = 0; i < 100; ++i) metadata.ICC_profile[i] = static_cast<char>(i * 3);
    metadata.micronsPerPixel = 0.25f;
    metadata.magnification   = 40.f;
    return metadata;
}
constexpr uint32_t REVISION = 7;

/// Where write_structure put each block.
struct Placed { Offset table, metadata, icc, images, attributes; };

/// Place everything after the tiles and seal. `metadata_first` reserves the
/// METADATA block right after the tile table and fills it once its children
/// exist (Iris-Codec's order: fixed-size blocks ahead of the ones that change);
/// otherwise METADATA is appended last. Either is legal — the order of the
/// calls is the order on disk.
Placed write_structure(const Builder& builder, const BuilderTileTableInfo& grid,
                       const Metadata& m, bool metadata_first = true) {
    Placed at{};
    const auto tiles  = builder.tile_offsets();
    // The grid is the caller's; normalise the plane counts as set_tile_table does.
    const auto planes = grid.planes.empty()
        ? std::vector<uint16_t>(grid.extent.layers.size(), 0) : grid.planes;
    std::vector<b::LayerExtentEntry> extents;
    extents.reserve(grid.extent.layers.size());
    for (std::size_t l = 0; l < grid.extent.layers.size(); ++l)
        extents.push_back({.X_TILES  = grid.extent.layers[l].xTiles,
                           .Y_TILES  = grid.extent.layers[l].yTiles,
                           .SCALE    = grid.extent.layers[l].scale,
                           .Z_PLANES = planes[l]});
    const Offset tiles_at   = builder->append(b::TileOffsetsCreateInfo{.entries = tiles});
    const Offset extents_at = builder->append(b::LayerExtentsCreateInfo{.entries = extents});
    at.table = builder->append(b::TileTableCreateInfo{
        .ENCODING             = static_cast<::Iris::File::constants::TileEncodings>(grid.encoding),
        .FORMAT               = static_cast<::Iris::File::constants::PixelFormats>(grid.format),
        .TILE_OFFSETS_OFFSET  = tiles_at,
        .LAYER_EXTENTS_OFFSET = extents_at,
        .X_EXTENT             = grid.extent.width,
        .Y_EXTENT             = grid.extent.height,
        .TILE_LENGTH          = grid.tileLength});
    if (metadata_first) at.metadata = builder->claim(b::METADATA::header_size);
    at.icc = builder->append(b::IccProfileCreateInfo{
        .bytes = reinterpret_cast<const BYTE*>(m.ICC_profile.data()),
        .count = m.ICC_profile.size()});
    const auto img_entries = builder.image_entries();
    at.images = img_entries.empty() ? ::Iris::File::constants::NULL_OFFSET
                                    : builder->append(b::ImagesCreateInfo{.entries = img_entries});
    if (m.attributes.empty()) at.attributes = ::Iris::File::constants::NULL_OFFSET;
    else {
        std::vector<b::AttributeSizeEntry> pairs;
        pairs.reserve(m.attributes.size());
        for (const auto& [key, value] : m.attributes)
            pairs.push_back({.key   = key,
                             .value = std::string(reinterpret_cast<const char*>(value.data()),
                                                  value.size())});
        const Offset sizes_at = builder->append(b::AttributeSizesCreateInfo{.entries = pairs});
        const Offset bytes_at = builder->append(b::AttributeBytesCreateInfo{.entries = pairs});
        at.attributes = builder->append(b::AttributesCreateInfo{
            .FORMAT       = static_cast<::Iris::File::constants::MetadataFormats>(m.attributes.type),
            .VERSION      = m.attributes.version,
            .SIZES_OFFSET = sizes_at,
            .BYTES_OFFSET = bytes_at});
    }
    const b::MetadataCreateInfo metadata{
        .CODEC_MAJOR       = static_cast<uint16_t>(m.codec.major),
        .CODEC_MINOR       = static_cast<uint16_t>(m.codec.minor),
        .CODEC_BUILD       = static_cast<uint16_t>(m.codec.build),
        .ATTRIBUTES_OFFSET = at.attributes,
        .IMAGES_OFFSET     = at.images,
        .ICC_COLOR_OFFSET  = at.icc,
        .MICRONS_PIXEL     = m.micronsPerPixel,
        .MAGNIFICATION     = m.magnification};
    if (metadata_first) builder->fill(at.metadata, metadata);
    else                at.metadata = builder->append(metadata);
    builder.finalize({.tileTable = at.table, .metadata = at.metadata, .revision = REVISION});
    return at;
}

AssociatedImageInfo label_image() {
    return AssociatedImageInfo{.imageLabel   = "label",
                               .width        = 64,
                               .height       = 32,
                               .encoding     = IMAGE_ENCODING_JPEG,
                               .sourceFormat = FORMAT_B8G8R8A8,
                               .orientation  = ORIENTATION_90};
}
std::vector<BYTE> image_bytes() {
    std::vector<BYTE> s(50);
    for (size_t j = 0; j < s.size(); ++j) s[j] = static_cast<BYTE>(0xA0 + j);
    return s;
}

/// Append all five tiles from `threads` workers in a shuffled order.
void append_all(const Builder& builder, unsigned threads, uint16_t layer1_planes = 0) {
    std::vector<uint32_t> order = {0, 1, 2, 3, 4};
    std::mt19937 rng(12345);
    std::shuffle(order.begin(), order.end(), rng);
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < threads; ++w)
        pool.emplace_back([&, w] {
            for (size_t i = w; i < order.size(); i += threads) {
                const uint32_t g = order[i];
                const auto s = stream_for(g);
                builder.append_tile(COORDS[g].layer, COORDS[g].tile, s.data(), s.size(),
                                    COORDS[g].layer == 1 ? layer1_planes : 0);
            }
        });
    for (auto& t : pool) t.join();
}

/// Read the file back and check every tile's bytes against stream_for().
void check_tiles(const FileAccessInfo& at, const Abstraction::File& file) {
    IFE_CHECK(file.tileTable.layers.size() == 2);
    if (file.tileTable.layers.size() != 2) return;
    for (uint32_t g = 0; g < 5; ++g) {
        const auto& entry = file.tileTable.layers[COORDS[g].layer][COORDS[g].tile];
        const auto expected = stream_for(g);
        IFE_CHECK(entry.size == expected.size());
        IFE_CHECK(entry.offset + entry.size <= at.file_size);
        if (entry.size == expected.size() && entry.offset + entry.size <= at.file_size)
            IFE_CHECK(std::memcmp(at.file_ptr + entry.offset, expected.data(), expected.size()) == 0);
    }
}

}  // namespace

int main() {
    // ---- (a) round trip, four threads, shuffled order ---------------------- //
    {
        const Builder builder = Builder::create({.capacity = 1ULL << 20});
        builder.set_tile_table(two_layers());
        append_all(builder, 4);
        const auto img = image_bytes();
        builder.append_image(label_image(), img.data(), img.size());
        const auto meta = some_metadata();
        write_structure(builder, two_layers(), meta);

        const FileAccessInfo at{builder->base(), builder.head()};
        IFE_CHECK(validate_file_structure(at) == IRIS_SUCCESS);
        const Abstraction::File file = abstract_file_structure(at);
        IFE_CHECK(file.header.fileSize == builder.head());
        IFE_CHECK(file.header.revision == REVISION);
        IFE_CHECK(file.tileTable.encoding == TILE_ENCODING_JPEG);
        IFE_CHECK(file.tileTable.format == FORMAT_R8G8B8A8);
        IFE_CHECK(file.tileTable.extent.width == 512 && file.tileTable.extent.height == 512);
        IFE_CHECK(file.tileTable.extent.layers.size() == 2);
        IFE_CHECK(file.tileTable.tileLength == 256);
        check_tiles(at, file);

        const auto image = file.images.find("label");
        IFE_CHECK(image != file.images.end());
        if (image != file.images.end()) {
            IFE_CHECK(image->second.info.width == 64);
            IFE_CHECK(image->second.info.height == 32);
            IFE_CHECK(image->second.info.encoding == IMAGE_ENCODING_JPEG);
            IFE_CHECK(image->second.info.sourceFormat == FORMAT_B8G8R8A8);
            IFE_CHECK(image->second.info.orientation == ORIENTATION_90);
            IFE_CHECK(image->second.byteSize == img.size());
            IFE_CHECK(std::memcmp(at.file_ptr + image->second.offset, img.data(), img.size()) == 0);
        }

        IFE_CHECK(file.metadata.codec.major == 2025 && file.metadata.codec.minor == 3 &&
                  file.metadata.codec.build == 2);
        IFE_CHECK(file.metadata.attributes.size() == 2);
        IFE_CHECK(file.metadata.attributes.at("a.key") == u8"one");
        IFE_CHECK(file.metadata.attributes.at("b.key") == u8"two");
        IFE_CHECK(file.metadata.ICC_profile == meta.ICC_profile);
        IFE_CHECK(file.metadata.micronsPerPixel == 0.25f);
        IFE_CHECK(file.metadata.magnification == 40.f);

        // ---- (b) frames at the anchor ------------------------------------- //
        for (uint32_t g = 0; g < 5; ++g) {
            const auto& entry = file.tileTable.layers[COORDS[g].layer][COORDS[g].tile];
            const b::TILE_PIXEL_DATA frame{at.file_ptr, entry.offset, at.file_size,
                                           b::VERSION_WRITTEN};
            IFE_CHECK(static_cast<bool>(frame.validate()));
            IFE_CHECK(frame.tile_index().value_or(~0u) == g);
        }
    }

    // ---- (c) NULL_TILE: a legitimate "no tile here" ------------------------ //
    {
        const Builder builder = Builder::create({.capacity = 1ULL << 20});
        builder.set_tile_table(two_layers());
        for (uint32_t g = 0; g < 5; ++g) {
            if (g == 3) { builder.append_null_tile(COORDS[g].layer, COORDS[g].tile); continue; }
            const auto s = stream_for(g);
            builder.append_tile(COORDS[g].layer, COORDS[g].tile, s.data(), s.size());
        }
        // Once accounted for, a position cannot be appended again.
        const auto s = stream_for(3);
        IFE_CHECK(throws<std::logic_error>([&] {
            builder.append_tile(COORDS[3].layer, COORDS[3].tile, s.data(), s.size()); }));
        write_structure(builder, two_layers(), some_metadata());

        const FileAccessInfo at{builder->base(), builder.head()};
        IFE_CHECK(validate_file_structure(at) == IRIS_SUCCESS);
        const auto file = abstract_file_structure(at);
        const auto& null_entry = file.tileTable.layers[COORDS[3].layer][COORDS[3].tile];
        IFE_CHECK(null_entry.offset == k::NULL_TILE);
        IFE_CHECK(null_entry.size == 0);
    }

    // ---- (d) a Z-stacked layer frames every stream ------------------------- //
    {
        const Builder builder = Builder::create({.capacity = 1ULL << 20, .tile_frames = false});
        builder.set_tile_table(two_layers({0, 3}));
        // A stream may carry fewer planes than its layer's maximum, never more.
        const auto s = stream_for(1);
        IFE_CHECK(throws<std::invalid_argument>([&] { builder.append_tile(1, 0, s.data(), s.size(), 4); }));
        const auto s0 = stream_for(0);
        IFE_CHECK(throws<std::invalid_argument>([&] { builder.append_tile(0, 0, s0.data(), s0.size(), 2); }));
        append_all(builder, 2, /*layer1_planes=*/3);
        write_structure(builder, two_layers({0, 3}), some_metadata());

        const FileAccessInfo at{builder->base(), builder.head()};
        IFE_CHECK(validate_file_structure(at) == IRIS_SUCCESS);
        const auto file = abstract_file_structure(at);
        check_tiles(at, file);
        IFE_CHECK(file.tileTable.planes.size() == 2);
        if (file.tileTable.planes.size() == 2) {
            IFE_CHECK(file.tileTable.planes[0] == 1);   // single plane reads back as 1
            IFE_CHECK(file.tileTable.planes[1] == 3);
        }
        for (uint32_t g = 0; g < 5; ++g) {
            const auto& entry = file.tileTable.layers[COORDS[g].layer][COORDS[g].tile];
            const b::TILE_PIXEL_DATA frame{at.file_ptr, entry.offset, at.file_size,
                                           b::VERSION_WRITTEN};
            const bool framed = static_cast<bool>(frame.validate());
            IFE_CHECK(framed == (COORDS[g].layer == 1));   // frames off; layer 1 framed anyway
            if (framed) {
                IFE_CHECK(frame.tile_index().value_or(~0u) == g);
                IFE_CHECK(frame.z_planes().value_or(0) == 3);
            }
        }
    }

    // ---- (e) misuse throws; a refused write writes nothing ----------------- //
    {
        const Builder builder = Builder::create({.capacity = 1ULL << 20});
        const auto s = stream_for(0);
        IFE_CHECK(throws<std::logic_error>([&] { builder.append_tile(0, 0, s.data(), s.size()); }));
        IFE_CHECK(throws<std::logic_error>([&] { builder.tile_offsets(); }));
        builder.set_tile_table(two_layers());
        IFE_CHECK(throws<std::logic_error>([&] { builder.set_tile_table(two_layers()); }));
        IFE_CHECK(throws<std::out_of_range>([&] { builder.append_tile(2, 0, s.data(), s.size()); }));
        IFE_CHECK(throws<std::out_of_range>([&] { builder.append_tile(1, 4, s.data(), s.size()); }));
        IFE_CHECK(throws<std::invalid_argument>([&] { builder.append_tile(0, 0, s.data(), 0); }));
        IFE_CHECK(throws<std::invalid_argument>([&] { builder.append_tile(0, 0, s.data(), Size{1} << 24); }));

        builder.append_tile(0, 0, s.data(), s.size());
        IFE_CHECK(throws<std::logic_error>([&] { builder.append_tile(0, 0, s.data(), s.size()); }));

        const auto img = image_bytes();
        builder.append_image(label_image(), img.data(), img.size());
        IFE_CHECK(throws<std::invalid_argument>([&] { builder.append_image(label_image(), img.data(), img.size()); }));

        // Tiles are still unwritten: the placement report refuses.
        const Offset before = builder.head();
        IFE_CHECK(throws<std::logic_error>([&] { builder.tile_offsets(); }));
        IFE_CHECK(builder.head() == before);
        for (uint32_t g = 1; g < 5; ++g) {
            const auto t = stream_for(g);
            builder.append_tile(COORDS[g].layer, COORDS[g].tile, t.data(), t.size());
        }

        // A fill must land inside claimed space, never in the header region.
        const b::MetadataCreateInfo empty_metadata{};
        IFE_CHECK(throws<std::out_of_range>([&] { builder->fill(builder.head(), empty_metadata); }));
        IFE_CHECK(throws<std::out_of_range>([&] { builder->fill(0, empty_metadata); }));
        // A header naming no roots is refused before it is written.
        IFE_CHECK(throws<std::invalid_argument>([&] { builder.finalize({}); }));

        // The caller composes the tile table from the placements.
        const auto grid   = two_layers();
        const auto tiles  = builder.tile_offsets();
        const auto planes = grid.planes.empty()
            ? std::vector<uint16_t>(grid.extent.layers.size(), 0) : grid.planes;
        std::vector<b::LayerExtentEntry> extents;
        extents.reserve(grid.extent.layers.size());
        for (std::size_t l = 0; l < grid.extent.layers.size(); ++l)
            extents.push_back({.X_TILES = grid.extent.layers[l].xTiles,
                               .Y_TILES = grid.extent.layers[l].yTiles,
                               .SCALE   = grid.extent.layers[l].scale,
                               .Z_PLANES = planes[l]});
        const Offset tiles_at   = builder->append(b::TileOffsetsCreateInfo{.entries = tiles});
        const Offset extents_at = builder->append(b::LayerExtentsCreateInfo{.entries = extents});
        const Offset table_at   = builder->append(b::TileTableCreateInfo{
            .ENCODING             = static_cast<::Iris::File::constants::TileEncodings>(grid.encoding),
            .FORMAT               = static_cast<::Iris::File::constants::PixelFormats>(grid.format),
            .TILE_OFFSETS_OFFSET  = tiles_at,
            .LAYER_EXTENTS_OFFSET = extents_at,
            .X_EXTENT             = grid.extent.width,
            .Y_EXTENT             = grid.extent.height,
            .TILE_LENGTH          = grid.tileLength});
        const Offset metadata_at = builder->append(empty_metadata);
        builder.finalize({.tileTable = table_at, .metadata = metadata_at});
        const FileAccessInfo at{builder->base(), builder.head()};
        IFE_CHECK(validate_file_structure(at) == IRIS_SUCCESS);
        IFE_CHECK(throws<std::logic_error>([&] {
            builder.finalize({.tileTable = table_at, .metadata = metadata_at}); }));
    }

    // ---- (h) the layout is the caller's ----------------------------------- //
    {
        // The same inputs in two orders: METADATA reserved right after the tile
        // table and filled last, or appended last. Each block lies where it was
        // put, and both files read back alike.
        Abstraction::File files[2];
        for (const bool metadata_first : {true, false}) {
            const Builder builder = Builder::create({.capacity = 1ULL << 20});
            builder.set_tile_table(two_layers());
            append_all(builder, 1);
            const auto img = image_bytes();
            builder.append_image(label_image(), img.data(), img.size());
            const Placed placed = write_structure(builder, two_layers(), some_metadata(), metadata_first);
            IFE_CHECK(placed.table < placed.icc && placed.icc < placed.images &&
                      placed.images < placed.attributes);
            IFE_CHECK(metadata_first ? (placed.table < placed.metadata && placed.metadata < placed.icc)
                                     : placed.attributes < placed.metadata);
            const FileAccessInfo at{builder->base(), builder.head()};
            IFE_CHECK(validate_file_structure(at) == IRIS_SUCCESS);
            files[metadata_first] = abstract_file_structure(at);
            check_tiles(at, files[metadata_first]);
        }
        IFE_CHECK(files[0].metadata.ICC_profile == files[1].metadata.ICC_profile);
        IFE_CHECK(files[0].metadata.attributes == files[1].metadata.attributes);
        IFE_CHECK(files[0].metadata.micronsPerPixel == files[1].metadata.micronsPerPixel);
        IFE_CHECK(files[0].images.size() == 1 && files[1].images.size() == 1);
    }

    // ---- (f) frames off round-trips, with no frames written ---------------- //
    {
        const Builder builder = Builder::create({.capacity = 1ULL << 20, .tile_frames = false});
        builder.set_tile_table(two_layers());
        append_all(builder, 3);
        write_structure(builder, two_layers(), some_metadata());
        const FileAccessInfo at{builder->base(), builder.head()};
        IFE_CHECK(validate_file_structure(at) == IRIS_SUCCESS);
        const auto file = abstract_file_structure(at);
        check_tiles(at, file);
        for (uint32_t g = 0; g < 5; ++g) {
            const auto& entry = file.tileTable.layers[COORDS[g].layer][COORDS[g].tile];
            const b::TILE_PIXEL_DATA frame{at.file_ptr, entry.offset, at.file_size,
                                           b::VERSION_WRITTEN};
            IFE_CHECK(!frame.validate());
        }
    }

    // ---- (g) file-backed: closed at finalize, moved by the caller ---------- //
    {
        namespace fs = std::filesystem;
        std::random_device entropy;
        const std::string tag = std::to_string(entropy()) + std::to_string(entropy());
        const fs::path written = fs::temp_directory_path() / ("ife_layered_" + tag + ".iris");
        const fs::path moved   = fs::temp_directory_path() / ("ife_layered_" + tag + "_moved.iris");
        {
            const Builder builder = Builder::create({.filepath = written});
            builder.set_tile_table(two_layers());
            append_all(builder, 4);
            write_structure(builder, two_layers(), some_metadata());
            std::error_code ec;
            IFE_CHECK(fs::file_size(written, ec) == builder.head());
            fs::rename(written, moved, ec);                 // while the handle lives
            IFE_CHECK(!ec);
        }
        Memory mapped;
        const Result opened = create_memory({.filepath = moved, .read_only = true}, mapped);
        IFE_CHECK(static_cast<bool>(opened));
        if (mapped) {
            const FileAccessInfo at{mapped.base(), static_cast<Size>(fs::file_size(moved))};
            IFE_CHECK(validate_file_structure(at) == IRIS_SUCCESS);
            check_tiles(at, abstract_file_structure(at));
            mapped.close();
        }
        std::error_code ec;
        fs::remove(written, ec);
        fs::remove(moved, ec);
    }

    if (g_failures == 0) std::printf("ife_builder_layered_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
