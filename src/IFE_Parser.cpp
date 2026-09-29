/**
 * @file IFE_Parser.cpp
 * @brief Implementation of the read handle/body declared in "IFE_Parser.hpp"
 *        and "IrisFileExtension.hpp".
 * @copyright Iris Developers, 2025-2026
 */

#include "IFE_Parser.hpp"
#include "IFE_Primitives.hpp"   // tile_frame_error
#include "IrisMemory.hpp"       // priv: Iris::Memory, for Parser::open

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Iris::File {

namespace k = ::Iris::File::constants;
namespace b = ::Iris::File::blocks;

namespace {

/// The entry for (layer, tile), or out_of_range naming what was asked for.
const Abstraction::TileEntry& entry_at(const Abstraction::File& __file,
                                       uint32_t __layer, uint32_t __tile) {
    const auto& layers = __file.tileTable.layers;
    if (__layer >= layers.size())
        throw std::out_of_range("IFE Parser: layer " + std::to_string(__layer) +
                                " of a slide with " + std::to_string(layers.size()) + " layers");
    if (__tile >= layers[__layer].size())
        throw std::out_of_range("IFE Parser: tile " + std::to_string(__tile) + " of layer " +
                                std::to_string(__layer) + ", which has " +
                                std::to_string(layers[__layer].size()) + " tiles");
    return layers[__layer][__tile];
}

/// A span over [offset, offset + size), or runtime_error if it leaves the file.
/// The abstraction has already checked every range it hands out; this is the
/// last line, so a span can never point past the mapping.
std::span<const BYTE> span_at(const FileAccessInfo& __info, Offset __offset, Size __size,
                              const std::string& __what) {
    // Never `__offset + __size <= file_size`: that sum can wrap.
    if (__offset > __info.file_size || __info.file_size - __offset < __size)
        throw std::runtime_error("IFE Parser: " + __what + " lies outside the file");
    return {__info.file_ptr + __offset, static_cast<std::size_t>(__size)};
}

}  // namespace

// =====================================================================
// Parser_t — the body
// =====================================================================

const Abstraction::File& Parser_t::abstraction() const
{
    // A throw leaves the flag unset, so a damaged file refuses on every call
    // rather than handing back a half-lifted structure.
    std::call_once(m_lifted, [this] { m_abstraction = ::Iris::File::abstract_file_structure(m_info); });
    return m_abstraction;
}

std::span<const BYTE> Parser_t::tile(uint32_t layer, uint32_t tile) const
{
    const Abstraction::TileEntry& entry = entry_at(abstraction(), layer, tile);
    if (entry.offset == k::NULL_TILE) return {};   // no tile at this grid position
    return span_at(m_info, entry.offset, entry.size,
                   "layer " + std::to_string(layer) + ", tile " + std::to_string(tile));
}

uint16_t Parser_t::tile_planes(uint32_t layer, uint32_t tile) const
{
    const Abstraction::File&       file  = abstraction();
    const Abstraction::TileEntry&  entry = entry_at(file, layer, tile);
    if (entry.offset == k::NULL_TILE) return 0;
    const uint16_t layer_planes = file.tileTable.planes[layer];   // normalised: at least 1
    if (layer_planes <= 1) return 1;

    // Every stream of a Z-stacked layer is framed, and abstraction() has
    // already refused a file where one is not; checked again because this is
    // the read that depends on it.
    const b::FILE_HEADER header = versioned_root(m_info.file_ptr, m_info.file_size);
    // Global indices ascend by layer, row-major within a layer.
    uint64_t global = tile;
    for (uint32_t l = 0; l < layer; ++l) global += file.tileTable.layers[l].size();
    if (const char* why = tile_frame_error(m_info.file_ptr, m_info.file_size, header.__version,
                                           entry.offset, global, layer_planes))
        throw std::runtime_error("IFE Parser: layer " + std::to_string(layer) + ", tile " +
                                 std::to_string(tile) + " " + why);
    const b::TILE_PIXEL_DATA frame{m_info.file_ptr, entry.offset, m_info.file_size, header.__version};
    return std::max<uint16_t>(frame.z_planes().value_or(0), 1);   // zero means one plane
}

std::span<const BYTE> Parser_t::image(const std::string& label) const
{
    const Abstraction::File& file = abstraction();
    const auto found = file.images.find(label);
    if (found == file.images.end())
        throw std::out_of_range("IFE Parser: no associated image labelled '" + label + "'");
    return span_at(m_info, found->second.offset, found->second.byteSize,
                   "associated image '" + label + "'");
}

// =====================================================================
// Parser — the handle
// =====================================================================

Parser::Parser(const FileAccessInfo& info)
    : Base(std::make_shared<Parser_t>(info)) {}

Parser Parser::open(const std::filesystem::path& path)
{
    auto memory = std::make_shared<Iris::Memory>();
    const Iris::Result made = Iris::create_memory(
        Iris::MemoryCreateInfo{.filepath = path, .read_only = true}, *memory);
    if (!made)
        throw std::runtime_error("IFE Parser: could not open " + path.string() + " (" +
                                 made.message + ")");
    // A read-only arena's capacity is the file's own size.
    const FileAccessInfo info{memory->base(), memory->capacity()};
    return Parser(std::make_shared<Parser_t>(info, std::shared_ptr<const void>(std::move(memory))));
}

Result                   Parser::is_iris_codec_file() const noexcept { return get()->is_iris_codec_file(); }
Result                   Parser::validate_file_structure() const noexcept { return get()->validate_file_structure(); }
Abstraction::File        Parser::abstract_file_structure() const { return get()->abstract_file_structure(); }
const Abstraction::File& Parser::abstraction() const { return get()->abstraction(); }
std::span<const BYTE>    Parser::tile(uint32_t layer, uint32_t tile) const { return get()->tile(layer, tile); }
uint16_t                 Parser::tile_planes(uint32_t layer, uint32_t tile) const { return get()->tile_planes(layer, tile); }
std::span<const BYTE>    Parser::image(const std::string& label) const { return get()->image(label); }
const FileAccessInfo&    Parser::info() const noexcept { return get()->info(); }
const BYTE*              Parser::data() const noexcept { return get()->data(); }
Size                     Parser::size() const noexcept { return get()->size(); }

}  // namespace Iris::File
