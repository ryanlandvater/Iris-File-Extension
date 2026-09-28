/**
 * @file IFE_Builder.cpp
 * @brief Implementation of the write handle/body declared in "IFE_Builder.hpp"
 *        and "IrisFileExtension.hpp".
 * @copyright Iris Developers, 2025-2026
 */

#include "IFE_Builder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace Iris::File {

namespace k = ::Iris::File::constants;
namespace b = ::Iris::File::blocks;

namespace {

/// The coordinate every tile error names.
std::string where(std::uint32_t __layer, std::uint32_t __tile) {
    return "IFE Builder: layer " + std::to_string(__layer) + ", tile " + std::to_string(__tile);
}

}  // namespace

// =====================================================================
// Builder_t — the body
// =====================================================================

Builder_t::Builder_t(const Iris::Memory& memory, const BuilderCreateInfo& info)
    : m_memory(memory),
      m_path(info.filepath),
      m_tile_frames(info.tile_frames),
      m_base(reinterpret_cast<::Iris::File::BYTE*>(memory.base()))
{
    // A builder writes into the mapping, so an empty one is a caller error,
    // not a stream to seal later.
    if (m_base == nullptr || m_memory.capacity() == 0)
        throw std::runtime_error("IFE Builder: the arena holds no writable mapping");
    // The head starts past the FILE_HEADER (see m_head), so the arena must at
    // least hold the header that seal will write there.
    if (m_memory.capacity() < b::FILE_HEADER::header_size)
        throw std::runtime_error(
            "IFE Builder: arena capacity (" + std::to_string(m_memory.capacity()) +
            " bytes) cannot hold the " + std::to_string(b::FILE_HEADER::header_size) +
            "-byte FILE_HEADER");
    // The header region is never claimed (the head starts past it), so it is
    // committed here: `seal` writes it, and a Windows anonymous arena faults on
    // a page nobody committed (Iris::Memory::commit).
    m_memory.commit(0, b::FILE_HEADER::header_size);
}

void Builder_t::ensure_open(const char* what) const
{
    if (m_sealed.load(std::memory_order_acquire))
        throw std::logic_error(std::string("IFE Builder: ") + what +
                               " after finalize; the builder accepts no more writes");
}

::Iris::File::Offset Builder_t::claim(::Iris::File::Size bytes)
{
    // Lock-free bump: load, bounds-check, CAS. A losing CAS re-reads and
    // retries, so concurrent callers each win a distinct whole range. The
    // bounds test precedes the CAS, so a claim that cannot fit never publishes
    // a head past capacity -- it throws instead, and the throw is the abort.
    ensure_open("claim");
    const ::Iris::File::Size cap = m_memory.capacity();
    ::Iris::File::Offset current = m_head.load(std::memory_order_relaxed);
    for (;;)
    {
        // Overflow-safe: `current > cap` first, so `cap - current` cannot wrap.
        if (current > cap || bytes > cap - current)
            throw std::runtime_error(
                "IFE Builder: arena capacity (" + std::to_string(cap) +
                " bytes) exhausted at head " + std::to_string(current) +
                " while claiming " + std::to_string(bytes) +
                "; reserve a larger BuilderCreateInfo::capacity");
        if (m_head.compare_exchange_weak(current, current + bytes,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
            // The range is ours; make it touchable before anyone writes it. A
            // no-op except on a Windows anonymous arena, which is reserved,
            // not committed (Iris::Memory::commit).
            m_memory.commit(current, bytes);
            return current;
        }
    }
}

// ---- the tile table ------------------------------------------------------ //

void Builder_t::set_tile_table(const BuilderTileTableInfo& info)
{
    ensure_open("set_tile_table");
    std::lock_guard<std::mutex> lock(m_table_mutex);
    if (m_table_ready.load(std::memory_order_acquire))
        throw std::logic_error("IFE Builder: set_tile_table called twice");

    const auto& layers = info.extent.layers;
    if (layers.empty())
        throw std::invalid_argument("IFE Builder: the tile table declares no layers");
    if (!info.planes.empty() && info.planes.size() != layers.size())
        throw std::invalid_argument(
            "IFE Builder: planes has " + std::to_string(info.planes.size()) +
            " entries for " + std::to_string(layers.size()) + " layers");

    // Global tile indices ascend by layer, row-major within a layer: the
    // first index of layer l is the number of tiles all preceding layers hold.
    std::vector<std::uint64_t> first(layers.size() + 1, 0);
    for (std::size_t l = 0; l < layers.size(); ++l)
        first[l + 1] = first[l] + static_cast<std::uint64_t>(layers[l].xTiles) * layers[l].yTiles;
    // The TILE_OFFSETS COUNT and a frame's TILE_INDEX are both u32.
    if (first.back() > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(
            "IFE Builder: " + std::to_string(first.back()) +
            " tiles exceed what a u32 global tile index can address");

    m_table       = info;
    m_planes      = info.planes.empty() ? std::vector<std::uint16_t>(layers.size(), 0)
                                        : info.planes;
    m_layer_first = std::move(first);
    m_slots       = std::make_unique<TileSlot[]>(m_layer_first.back());
    m_table_ready.store(true, std::memory_order_release);
}

Builder_t::TileSlot& Builder_t::slot(std::uint32_t layer, std::uint32_t tile, std::uint64_t& global)
{
    if (!m_table_ready.load(std::memory_order_acquire))
        throw std::logic_error("IFE Builder: append a tile only after set_tile_table");
    if (static_cast<std::size_t>(layer) + 1 >= m_layer_first.size())
        throw std::out_of_range(where(layer, tile) + ": the slide has " +
                                std::to_string(m_layer_first.size() - 1) + " layers");
    const std::uint64_t count = m_layer_first[layer + 1] - m_layer_first[layer];
    if (tile >= count)
        throw std::out_of_range(where(layer, tile) + ": the layer has " +
                                std::to_string(count) + " tiles");
    global = m_layer_first[layer] + tile;
    return m_slots[global];
}

Offset Builder_t::append_tile(std::uint32_t layer, std::uint32_t tile, const BYTE* data,
                              Size size, std::uint16_t z_planes)
{
    ensure_open("append_tile");
    std::uint64_t global = 0;
    TileSlot& s = slot(layer, tile, global);

    if (data == nullptr || size == 0)
        throw std::invalid_argument(where(layer, tile) +
                                    ": an empty stream (append_null_tile records no tile)");
    // SIZE is u24 on the wire: a larger stream would be silently truncated.
    if (size > 0xFFFFFF)
        throw std::invalid_argument(where(layer, tile) + ": a " + std::to_string(size) +
                                    "-byte stream; a tile stream is at most 16777215 bytes");

    // A Z-stacked layer frames every stream, because the frame is the only
    // place the format records how many planes a stream carries; a stream
    // may carry fewer than the layer's maximum, never more.
    const std::uint16_t layer_planes = m_planes[layer];
    const bool          z_stacked    = layer_planes > 1;
    if (z_stacked ? z_planes > layer_planes : z_planes > 1)
        throw std::invalid_argument(where(layer, tile) + ": " + std::to_string(z_planes) +
                                    " planes in a layer of at most " +
                                    std::to_string(z_stacked ? layer_planes : 1));

    // Cheap early refusal; the CAS below is the authority.
    if (s.offset.load(std::memory_order_acquire) != UNWRITTEN)
        throw std::logic_error(where(layer, tile) + ": already appended");

    Offset anchor = 0;
    if (z_stacked || m_tile_frames) {
        const Offset at = claim(b::TILE_PIXEL_DATA::header_size + size);
        anchor = at + b::TILE_PIXEL_DATA::header_size;
        // store() takes the ANCHOR — the stream's first byte, the one the tile
        // offsets entry names — and lays the frame out backward from it
        // (CLAUDE.md, "TILE_PIXEL_DATA grows backwards"). Passing `at` would
        // write a self-consistent frame five bytes early, attached to nothing.
        const b::Status status = b::store(m_base, anchor, b::TilePixelDataCreateInfo{
            .TILE_INDEX = static_cast<std::uint32_t>(global),
            .Z_PLANES   = z_planes});
        if (!status)
            throw std::runtime_error(where(layer, tile) + ": the tile frame failed to store (field '" +
                                     std::string(status.field) + "')");
    } else {
        anchor = claim(size);
    }
    // OFFSET is u40, and its maximum is NULL_TILE.
    if (anchor + size > k::NULL_TILE)
        throw std::runtime_error(where(layer, tile) +
                                 ": the stream lies past the 40-bit tile offset range");

    std::memcpy(m_base + anchor, data, size);

    std::uint64_t expected = UNWRITTEN;
    if (!s.offset.compare_exchange_strong(expected, anchor, std::memory_order_acq_rel,
                                          std::memory_order_acquire))
        throw std::logic_error(where(layer, tile) + ": already appended");
    s.size = static_cast<std::uint32_t>(size);
    return anchor;
}

void Builder_t::append_null_tile(std::uint32_t layer, std::uint32_t tile)
{
    ensure_open("append_null_tile");
    std::uint64_t global = 0;
    TileSlot& s = slot(layer, tile, global);
    std::uint64_t expected = UNWRITTEN;
    if (!s.offset.compare_exchange_strong(expected, k::NULL_TILE, std::memory_order_acq_rel,
                                          std::memory_order_acquire))
        throw std::logic_error(where(layer, tile) + ": already appended");
    s.size = 0;
}

// ---- associated images --------------------------------------------------- //

Offset Builder_t::append_image(const AssociatedImageInfo& info, const BYTE* data, Size size)
{
    ensure_open("append_image");
    const std::string& label = info.imageLabel;
    const std::string  what  = "IFE Builder: image '" + label + "'";
    if (data == nullptr || size == 0)
        throw std::invalid_argument(what + ": an empty image stream");
    // TITLE_SIZE is u16 and IMAGE_SIZE u32 on the wire.
    if (label.size() > 0xFFFF)
        throw std::invalid_argument(what + ": the label exceeds 65535 bytes");
    if (size > 0xFFFFFFFF)
        throw std::invalid_argument(what + ": the stream exceeds 4294967295 bytes");
    {
        // The read side keys images by label, so a second one would be lost.
        std::lock_guard<std::mutex> lock(m_images_mutex);
        if (!m_image_labels.insert(label).second)
            throw std::invalid_argument(what + ": appended twice");
    }

    // The generated size_of covers the IMAGE_BYTES header only; the label and
    // the stream follow it, so the claim adds both.
    const Offset at = claim(b::IMAGE_BYTES::header_size + label.size() + size);
    const b::Status status = b::store(m_base, at, b::ImageBytesCreateInfo{
        .TITLE_SIZE = static_cast<std::uint16_t>(label.size()),
        .IMAGE_SIZE = static_cast<std::uint32_t>(size)});
    if (!status)
        throw std::runtime_error(what + ": IMAGE_BYTES failed to store (field '" +
                                 std::string(status.field) + "')");
    BYTE* const payload = m_base + at + b::IMAGE_BYTES::header_size;
    std::memcpy(payload, label.data(), label.size());
    std::memcpy(payload + label.size(), data, size);

    std::lock_guard<std::mutex> lock(m_images_mutex);
    m_images.push_back(b::ImageEntry{
        .BYTES_OFFSET = at,
        .WIDTH        = info.width,
        .HEIGHT       = info.height,
        .ENCODING     = static_cast<k::ImageEncodings>(info.encoding),
        .FORMAT       = static_cast<k::PixelFormats>(info.sourceFormat),
        // The codec vocabulary holds orientation as binary16 bits; the entry
        // takes degrees, and store() encodes them back to the same bits.
        .ORIENTATION  = half_to_float(static_cast<std::uint16_t>(info.orientation))});
    return at;
}

// ---- finalize and seal --------------------------------------------------- //

void Builder_t::finalize(const BuilderFinalizeInfo& info)
{
    ensure_open("finalize");
    if (!m_table_ready.load(std::memory_order_acquire))
        throw std::logic_error("IFE Builder: finalize before set_tile_table; a slide needs a tile table");

    // Check every input before writing anything, so a refused finalize leaves
    // no half-written structure behind.
    const std::uint64_t total = m_layer_first.back();
    for (std::uint64_t g = 0; g < total; ++g) {
        if (m_slots[g].offset.load(std::memory_order_acquire) != UNWRITTEN) continue;
        // A tile nobody accounted for — a failed worker, an interrupted encode —
        // is reported, never published as NULL_TILE ("no tile here").
        const auto layer = static_cast<std::uint32_t>(
            std::upper_bound(m_layer_first.begin(), m_layer_first.end(), g) -
            m_layer_first.begin() - 1);
        throw std::logic_error(where(layer, static_cast<std::uint32_t>(g - m_layer_first[layer])) +
                               ": never appended (append_null_tile records that a "
                               "position has no tile)");
    }
    const Metadata& metadata   = info.metadata;
    const auto&     attributes = metadata.attributes;
    if (!attributes.empty() && attributes.type == METADATA_UNDEFINED)
        throw std::invalid_argument("IFE Builder: the attributes declare no format (METADATA_UNDEFINED)");

    // ---- the tile table ------------------------------------------------ //
    std::vector<b::TileOffsetEntry> tiles(total);
    for (std::uint64_t g = 0; g < total; ++g)
        tiles[g] = {.OFFSET = m_slots[g].offset.load(std::memory_order_acquire),
                    .SIZE   = m_slots[g].size};
    const Offset tiles_at = append(b::TileOffsetsCreateInfo{.entries = tiles});

    const auto& layers = m_table.extent.layers;
    std::vector<b::LayerExtentEntry> extents;
    extents.reserve(layers.size());
    for (std::size_t l = 0; l < layers.size(); ++l)
        extents.push_back({.X_TILES  = layers[l].xTiles,
                           .Y_TILES  = layers[l].yTiles,
                           .SCALE    = layers[l].scale,
                           .Z_PLANES = m_planes[l]});
    const Offset extents_at = append(b::LayerExtentsCreateInfo{.entries = extents});

    const Offset table_at = append(b::TileTableCreateInfo{
        .ENCODING             = static_cast<k::TileEncodings>(m_table.encoding),
        .FORMAT               = static_cast<k::PixelFormats>(m_table.format),
        .TILE_OFFSETS_OFFSET  = tiles_at,
        .LAYER_EXTENTS_OFFSET = extents_at,
        .X_EXTENT             = m_table.extent.width,
        .Y_EXTENT             = m_table.extent.height,
        .TILE_LENGTH          = m_table.tileLength});

    // ---- the blocks metadata points at --------------------------------- //
    Offset icc_at = k::NULL_OFFSET;
    if (!metadata.ICC_profile.empty())
        icc_at = append(b::IccProfileCreateInfo{
            .bytes = reinterpret_cast<const BYTE*>(metadata.ICC_profile.data()),
            .count = metadata.ICC_profile.size()});

    Offset images_at = k::NULL_OFFSET;
    {
        std::lock_guard<std::mutex> lock(m_images_mutex);
        if (!m_images.empty())
            images_at = append(b::ImagesCreateInfo{.entries = m_images});
    }

    Offset attributes_at = k::NULL_OFFSET;
    if (!attributes.empty()) {
        std::vector<b::AttributeSizeEntry> pairs;
        pairs.reserve(attributes.size());
        for (const auto& [key, value] : attributes)
            pairs.push_back({.key   = key,
                             .value = std::string(reinterpret_cast<const char*>(value.data()),
                                                  value.size())});
        // Sorted, so the same metadata always produces the same bytes; the
        // read side keys attributes by name, so order carries no meaning.
        std::sort(pairs.begin(), pairs.end(),
                  [](const auto& a, const auto& z) { return a.key < z.key; });
        // The generated writer derives both the sizes array and the packed byte
        // run from one payload, so the slicing cannot drift from the bytes.
        const Offset sizes_at = append(b::AttributeSizesCreateInfo{.entries = pairs});
        const Offset bytes_at = append(b::AttributeBytesCreateInfo{.entries = pairs});
        attributes_at = append(b::AttributesCreateInfo{
            .FORMAT       = static_cast<k::MetadataFormats>(attributes.type),
            .VERSION      = attributes.version,
            .SIZES_OFFSET = sizes_at,
            .BYTES_OFFSET = bytes_at});
    }

    const Offset metadata_at = append(b::MetadataCreateInfo{
        .CODEC_MAJOR       = static_cast<std::uint16_t>(metadata.codec.major),
        .CODEC_MINOR       = static_cast<std::uint16_t>(metadata.codec.minor),
        .CODEC_BUILD       = static_cast<std::uint16_t>(metadata.codec.build),
        .ATTRIBUTES_OFFSET = attributes_at,
        .IMAGES_OFFSET     = images_at,
        .ICC_COLOR_OFFSET  = icc_at,
        .MICRONS_PIXEL     = metadata.micronsPerPixel,
        .MAGNIFICATION     = metadata.magnification,
        .MICRONS_PLANE     = info.micronsPerPlane});

    seal(b::FileHeaderCreateInfo{
        .FILE_REVISION     = info.revision,
        .TILE_TABLE_OFFSET = table_at,
        .METADATA_OFFSET   = metadata_at});
}

void Builder_t::seal(::Iris::File::blocks::FileHeaderCreateInfo header)
{
    ensure_open("seal");

    // The one field the builder owns. The caller supplies every other header
    // field (the tile-table and metadata offsets they placed), but not this:
    // the committed head IS the file size, and restating it invites a mismatch.
    header.FILE_SIZE = static_cast<std::uint64_t>(m_head);

    const b::Status status = b::store(m_base, 0, header);
    if (!status)
        throw std::runtime_error(
            std::string("IFE Builder: seal failed to write the FILE_HEADER (block '") +
            status.block + "', field '" + status.field + "')");

    m_sealed.store(true, std::memory_order_release);

    // Anonymous: nothing is on disk. The bytes stay mapped and readable.
    if (m_path.empty()) return;

    // File-backed: release the mapping FIRST, then settle the file's length
    // from the reservation to the payload. Windows refuses SetEndOfFile on a
    // file with a mapped view (the old truncate-while-mapped failed there, and
    // no test truncated a file-backed arena, so CI never saw it); POSIX would
    // allow it, but one order for every platform is simpler. The file is then
    // complete and closed — what a caller needs to move it. Without the
    // truncate the file fails its own validate_file_structure, which compares
    // FILE_SIZE against the size the OS reports.
    const auto size = static_cast<std::uintmax_t>(m_head.load(std::memory_order_acquire));
    m_memory.close();
    m_base = nullptr;
    std::error_code error;
    std::filesystem::resize_file(m_path, size, error);
    if (error)
        throw std::system_error(error, "IFE Builder: could not truncate " +
                                       m_path.string() + " to its written size");
}

// =====================================================================
// Builder — the handle
// =====================================================================

Builder Builder::create(const BuilderCreateInfo& info)
{
    Iris::MemoryCreateInfo arena_info;
    arena_info.capacity = info.capacity;
    arena_info.filepath = info.filepath;
    // read_only defaults false: a builder needs a writable mapping.

    Iris::Memory memory;
    const Iris::Result made = Iris::create_memory(arena_info, memory);
    if (!made)
        throw std::runtime_error("IFE Builder: could not create the arena (" +
                                 std::string(made.message) + ")");

    return Builder(std::make_shared<Builder_t>(memory, info));
}

void   Builder::set_tile_table(const BuilderTileTableInfo& info) const { get()->set_tile_table(info); }
Offset Builder::append_tile(uint32_t layer, uint32_t tile, const BYTE* data, Size size,
                            uint16_t z_planes) const {
    return get()->append_tile(layer, tile, data, size, z_planes);
}
void   Builder::append_null_tile(uint32_t layer, uint32_t tile) const { get()->append_null_tile(layer, tile); }
Offset Builder::append_image(const AssociatedImageInfo& info, const BYTE* data, Size size) const {
    return get()->append_image(info, data, size);
}
void   Builder::finalize(const BuilderFinalizeInfo& info) const { get()->finalize(info); }
::Iris::File::Offset Builder::head()     const noexcept { return get()->head(); }
Size                 Builder::capacity() const noexcept { return get()->capacity(); }

}  // namespace Iris::File
