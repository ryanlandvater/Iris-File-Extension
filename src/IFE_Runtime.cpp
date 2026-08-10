/**
 * @file IFE_Runtime.cpp
 * @brief The semantic layer, ported onto the generated block handles.
 * @copyright Iris Developers, 2025-2026
 *
 * What disappeared relative to src/IrisCodecExtension.cpp, and why the port is
 * worth the churn: every byte offset, every `LOAD_U*`, every hand-threaded
 * `if (__version > IRIS_EXTENSION_1_0); else goto ...`, and every
 * `#ifdef __EMSCRIPTEN__`. A reader body here is a sequence of accessor calls.
 *
 * What stays hand-written is the part that is genuinely semantic: the
 * traversal order, which blocks are optional, how a flat tile-offset array is
 * split across layers, the downsample computation, and what a recovery scan
 * looks for. None of that is derivable from a byte layout.
 */

#include "IFE_Runtime.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace IrisCodec {

namespace k  = ::IFE::constants;
namespace b  = ::IFE::blocks;

namespace {

/// The single place a generated Status becomes an Iris::Result.
///
/// Every std::string in this layer is built here. Generated validators report
/// a code plus operands and never format anything (they are noexcept and
/// allocation-free); turning that into prose is the runtime's job, and doing
/// it in one function is what keeps the wording consistent across sixteen
/// blocks without sixteen message templates.
Result to_result(const b::Status& __status) noexcept try {
    if (__status) return IRIS_SUCCESS;

    const std::string where = std::string(__status.block) +
                              (*__status.field ? std::string(".") + __status.field : "") +
                              " at byte " + std::to_string(__status.at);
    switch (__status.code) {
        case b::Check::NOT_CONSTRUCTED:
            return {IRIS_FAILURE, where + " could not be read: the block does not fit within the file"};
        case b::Check::OUT_OF_BOUNDS:
            return {IRIS_FAILURE, where + " points outside the file"};
        case b::Check::BAD_VALIDATION:
            return {IRIS_FAILURE, where + " stores " + std::to_string(__status.found) +
                                  " as its own offset but sits at " + std::to_string(__status.expected) +
                                  "; the pointer that led here is wrong, or the block was moved"};
        case b::Check::BAD_RECOVERY:
            return {IRIS_FAILURE, where + " carries recovery tag " + std::to_string(__status.found) +
                                  " where " + std::to_string(__status.expected) + " was expected"};
        case b::Check::BAD_CONSTANT:
            return {IRIS_FAILURE, where + " holds " + std::to_string(__status.found) +
                                  " instead of the constant " + std::to_string(__status.expected)};
        case b::Check::BAD_STRIDE:
            return {IRIS_FAILURE, where + " declares a stride of " + std::to_string(__status.found) +
                                  ", narrower than the " + std::to_string(__status.expected) +
                                  " bytes an entry requires"};
        case b::Check::ARRAY_OVERRUN:
            return {IRIS_FAILURE, where + " declares " + std::to_string(__status.found) +
                                  " bytes of entries but only " + std::to_string(__status.expected) +
                                  " remain in the file"};
        case b::Check::CYCLE:
            return {IRIS_FAILURE, where + " is reached by an offset chain that returns to a block "
                                  "already on the path"};
        case b::Check::CONFORMANCE:
            return {IRIS_FAILURE, where + " violates a normative requirement of the specification"};
        case b::Check::OK: break;
    }
    return {IRIS_FAILURE, where + " failed validation"};
} catch (const std::bad_alloc&) {
    // noexcept, and the only thing above that can throw is the string building.
    return {IRIS_FAILURE, "validation failed (out of memory formatting the diagnostic)"};
}

[[noreturn]] void fail(const b::Status& __status) {
    throw std::runtime_error(to_result(__status).message);
}

/// The root handle. Its own version is unknowable until it has been read, so
/// it is constructed with UINT32_MAX — every gate open for exactly one block —
/// exactly as v1 does (IrisCodecExtension.cpp, FILE_HEADER's constructor).
b::FILE_HEADER root_at(const BYTE* __base, size_t __size) noexcept {
    return b::FILE_HEADER{__base, 0, __size, UINT32_MAX};
}

/// Compose the file's version the way v1 does: major << 16 | minor, read once
/// at the root and propagated to every child by construction.
uint32_t version_of(const b::FILE_HEADER& __header) noexcept {
    return (static_cast<uint32_t>(__header.extension_major()) << 16) | __header.extension_minor();
}

b::FILE_HEADER versioned_root(const BYTE* __base, size_t __size) noexcept {
    const b::FILE_HEADER bootstrap = root_at(__base, __size);
    return b::FILE_HEADER{__base, 0, __size, version_of(bootstrap)};
}

}  // namespace

// MARK: - Entry points

bool is_Iris_Codec_file(BYTE* const __base, size_t __size) {
    // MAGIC is a `constant` field, so the generated layer validates it rather
    // than handing it back: FILE_HEADER::validate() checks the magic number,
    // the recovery tag, and that the header fits -- exactly the two loads v1
    // performed, plus the bounds check it had to be given separately.
    return static_cast<bool>(root_at(__base, __size).validate());
}

Result validate_file_structure(BYTE* const __base, size_t __size) noexcept {
    const b::FILE_HEADER header = versioned_root(__base, __size);

    // v1 validated the header, then the tile table, then the metadata, each
    // with validate_full, early-returning on failure. validate_deep does the
    // same walk plus cycle detection, and follows edges leaving array entries
    // as well as block headers -- which v1's chain did not.
    return to_result(header.validate_deep());
}

Abstraction::File abstract_file_structure(BYTE* const __base, size_t __size) {
    using namespace Abstraction;
    File abstraction;

    const b::FILE_HEADER header = versioned_root(__base, __size);
    if (!header) fail(header.validate());

    abstraction.header = {.fileSize   = header.file_size(),
                          .extVersion = version_of(header),
                          .revision   = header.file_revision()};

    // ---- tile table ------------------------------------------------------ //
    const auto table = header.tile_table_offset();
    if (!table) fail(table.validate());

    abstraction.tileTable.encoding      = static_cast<Encoding>(table.encoding());
    abstraction.tileTable.format        = static_cast<Format>(table.format());
    abstraction.tileTable.extent.width  = table.x_extent();
    abstraction.tileTable.extent.height = table.y_extent();
    // Absent before 1.1, and zero means the same thing as absent, so both
    // normalise to the default here. The abstraction states the tile size
    // there is; it does not make the caller decode the two ways the file can
    // say "256".
    if (const auto size = table.tile_size(); size && *size != 0)
        abstraction.tileTable.tileSize = *size;

    const auto extents = table.layer_extents_offset();
    if (!extents) fail(extents.validate());
    abstraction.tileTable.extent.layers.resize(extents.count());
    abstraction.tileTable.planes.resize(extents.count());
    for (uint32_t i = 0; i < extents.count(); ++i) {
        auto& layer      = abstraction.tileTable.extent.layers[i];
        const auto entry = extents.entry(i);
        layer.xTiles     = entry.x_tiles();
        layer.yTiles     = entry.y_tiles();
        layer.scale      = entry.scale();
        // Same normalisation, same reason: one plane unless the file says more.
        abstraction.tileTable.planes[i] =
            std::max<uint16_t>(entry.planes().value_or(0), 1);
    }
    // Downsample is derived, not stored: the reciprocal of the scale relative
    // to the most magnified layer. Semantic, so it stays hand-written.
    if (!abstraction.tileTable.extent.layers.empty()) {
        const float max_scale = abstraction.tileTable.extent.layers.back().scale;
        for (auto& layer : abstraction.tileTable.extent.layers)
            layer.downsample = layer.scale != 0.f ? max_scale / layer.scale : 0.f;
    }

    // The tile offset array is flat; the layer extents say how to split it.
    const auto offsets = table.tile_offsets_offset();
    if (!offsets) fail(offsets.validate());

    uint64_t total_tiles = 0;
    for (const auto& layer : abstraction.tileTable.extent.layers)
        total_tiles += static_cast<uint64_t>(layer.xTiles) * layer.yTiles;
    if (total_tiles != offsets.count())
        throw std::runtime_error(
            "Tile count from the layer extents (" + std::to_string(total_tiles) +
            ") does not match the tile offset array (" + std::to_string(offsets.count()) + ")");

    abstraction.tileTable.layers.resize(abstraction.tileTable.extent.layers.size());
    uint32_t tile = 0;
    for (size_t li = 0; li < abstraction.tileTable.extent.layers.size(); ++li) {
        const auto& extent = abstraction.tileTable.extent.layers[li];
        auto&       layer  = abstraction.tileTable.layers[li];
        layer.resize(static_cast<size_t>(extent.xTiles) * extent.yTiles);
        for (auto& entry : layer) {
            const auto stored = offsets.entry(tile++);
            entry.offset = stored.offset();
            entry.size   = stored.size_field();
        }
    }

    // ---- metadata, and the optional blocks it points at ------------------ //
    const auto metadata = header.metadata_offset();
    if (!metadata) fail(metadata.validate());

    auto& meta = abstraction.metadata;
    meta.codec = {static_cast<uint16_t>(metadata.codec_major()),
                  static_cast<uint16_t>(metadata.codec_minor()),
                  static_cast<uint16_t>(metadata.codec_build())};
    meta.micronsPerPixel = metadata.microns_pixel();
    meta.magnification   = metadata.magnification();

    if (const auto attributes = metadata.attributes_offset()) {
        meta.attributes.type    = static_cast<MetadataType>(attributes.format());
        meta.attributes.version = attributes.version();

        const auto sizes = attributes.sizes_offset();
        const auto bytes = attributes.bytes_offset();
        if (!sizes) fail(sizes.validate());
        if (!bytes) fail(bytes.validate());

        // Keys and values are one byte run sliced by a parallel size array --
        // there is no string type in IFE, by design.
        const ::IFE::ByteSpan blob = bytes.bytes();
        Size cursor = 0;
        for (uint32_t i = 0; i < sizes.count(); ++i) {
            const auto entry     = sizes.entry(i);
            const Size key_size  = entry.key_size();
            const Size value_size = entry.value_size();
            if (cursor + key_size + value_size > blob.size)
                throw std::runtime_error(
                    "Attribute " + std::to_string(i) + " extends past the attribute byte array");

            const char* key = reinterpret_cast<const char*>(blob.data + cursor);
            cursor += key_size;
            const char8_t* value = reinterpret_cast<const char8_t*>(blob.data + cursor);
            cursor += value_size;
            meta.attributes[std::string(key, key_size)] = std::u8string(value, value_size);
        }
    }

    if (const auto images = metadata.images_offset()) {
        for (uint32_t i = 0; i < images.count(); ++i) {
            const auto entry = images.entry(i);
            const auto bytes = entry.bytes_offset();
            if (!bytes) fail(bytes.validate());

            // The label is the first TITLE_SIZE bytes of the image block; the
            // encoded stream is the IMAGE_SIZE bytes that follow it.
            const Size  title_size = bytes.title_size();
            const auto  payload    = bytes.__offset + b::IMAGE_BYTES::header_size;
            std::string label(reinterpret_cast<const char*>(__base + payload), title_size);

            AssociatedImage image;
            image.offset            = payload + title_size;
            image.byteSize          = bytes.image_size();
            image.info.imageLabel   = label;
            image.info.width        = entry.width();
            image.info.height       = entry.height();
            image.info.encoding     = static_cast<ImageEncoding>(entry.encoding());
            image.info.sourceFormat = static_cast<Format>(entry.format());
            image.info.orientation  =
                static_cast<AssociatedImageInfo::Orientation>(entry.orientation());

            meta.associatedImages.insert(label);
            abstraction.images[std::move(label)] = std::move(image);
        }
    }

    if (const auto profile = metadata.icc_color_offset()) {
        const ::IFE::ByteSpan bytes = profile.bytes();
        meta.ICC_profile.assign(reinterpret_cast<const char*>(bytes.data), bytes.size);
    }

    if (const auto clinical = metadata.clinical_offset()) {
        abstraction.clinicalOffset = clinical.__offset + b::CLINICAL_METADATA::header_size;
        abstraction.clinicalSize   = clinical.count();
    }

    if (const auto plane = metadata.microns_plane()) abstraction.micronsPerPlane = *plane;

    if (const auto annotations = metadata.annotations_offset()) {
        for (uint32_t i = 0; i < annotations.count(); ++i) {
            const auto entry = annotations.entry(i);
            const auto bytes = entry.bytes_offset();
            if (!bytes) fail(bytes.validate());

            Abstraction::Annotation note;
            note.offset    = bytes.__offset + b::ANNOTATION_BYTES::header_size;
            note.byteSize  = bytes.count();
            note.type      = static_cast<AnnotationTypes>(entry.format());
            note.xLocation = entry.x_location();
            note.yLocation = entry.y_location();
            note.xSize     = entry.x_size();
            note.ySize     = entry.y_size();
            note.width     = entry.pixel_width();
            note.height    = entry.pixel_height();
            note.parent    = entry.parent_id();

            const auto identifier = entry.identifier();
            abstraction.annotations[identifier] = note;
            meta.annotations.insert(identifier);
        }

        // Group titles are a byte run sliced by a parallel size array, as the
        // attributes are; each group's member identifiers follow its title.
        const auto sizes = annotations.group_sizes_offset();
        const auto blob  = annotations.group_bytes_offset();
        if (sizes && blob) {
            const ::IFE::ByteSpan titles = blob.bytes();
            Size cursor = 0;
            for (uint32_t i = 0; i < sizes.count(); ++i) {
                const auto entry      = sizes.entry(i);
                const Size title_size = entry.title_size();
                const Size members    = entry.member_count();
                if (cursor + title_size + members * 3 > titles.size)
                    throw std::runtime_error(
                        "Annotation group " + std::to_string(i) +
                        " extends past the annotation group byte array");

                std::string title(reinterpret_cast<const char*>(titles.data + cursor), title_size);
                cursor += title_size;

                Abstraction::AnnotationGroup group;
                group.offset = blob.__offset + b::ANNOTATION_GROUP_BYTES::header_size + cursor;
                group.number = static_cast<uint32_t>(members);
                cursor += members * 3;   // 24-bit identifiers

                meta.annotationGroups.insert(title);
                abstraction.annotations.groups[std::move(title)] = group;
            }
        }
    }

    return abstraction;
}

// MARK: - File mapping

namespace {

/// Record one block, keyed by offset. `size` is what the block occupies,
/// header and payload together.
void note(Abstraction::FileMap& __map, Abstraction::MapEntryType __type,
          Offset __offset, Size __size) {
    __map[__offset] = {.type = __type, .offset = __offset, .size = __size};
}

template <typename Block>
Size array_span(const Block& __b) noexcept {
    return Block::header_size + static_cast<Size>(__b.stride()) * __b.count();
}

template <typename Block>
Size blob_span(const Block& __b) noexcept {
    return Block::header_size + __b.count();
}

}  // namespace

Abstraction::FileMap generate_file_map(BYTE* const __base, size_t __size) {
    using namespace Abstraction;
    FileMap map;
    map.file_size = __size;

    const b::FILE_HEADER header = versioned_root(__base, __size);
    if (!header) fail(header.validate());
    note(map, MAP_ENTRY_FILE_HEADER, header.__offset, b::FILE_HEADER::header_size);

    const auto table = header.tile_table_offset();
    if (!table) fail(table.validate());
    note(map, MAP_ENTRY_TILE_TABLE, table.__offset, b::TILE_TABLE::header_size);

    if (const auto cipher = table.cipher_offset())
        note(map, MAP_ENTRY_CIPHER, cipher.__offset, b::CIPHER::header_size);

    if (const auto extents = table.layer_extents_offset())
        note(map, MAP_ENTRY_LAYER_EXTENTS, extents.__offset, array_span(extents));

    if (const auto offsets = table.tile_offsets_offset()) {
        note(map, MAP_ENTRY_TILE_OFFSETS, offsets.__offset, array_span(offsets));
        // The tile data itself is unframed -- it carries no block header, so
        // it can only be located through the entries that address it.
        for (uint32_t i = 0; i < offsets.count(); ++i) {
            const auto entry = offsets.entry(i);
            if (entry.offset() != k::NULL_TILE && entry.size_field() != 0)
                note(map, MAP_ENTRY_TILE_DATA, entry.offset(), entry.size_field());
        }
    }

    const auto metadata = header.metadata_offset();
    if (!metadata) fail(metadata.validate());
    note(map, MAP_ENTRY_METADATA, metadata.__offset, b::METADATA::header_size);

    if (const auto attributes = metadata.attributes_offset()) {
        note(map, MAP_ENTRY_ATTRIBUTES, attributes.__offset, b::ATTRIBUTES::header_size);
        if (const auto sizes = attributes.sizes_offset())
            note(map, MAP_ENTRY_ATTRIBUTE_SIZES, sizes.__offset, array_span(sizes));
        if (const auto bytes = attributes.bytes_offset())
            note(map, MAP_ENTRY_ATTRIBUTES_BYTES, bytes.__offset, blob_span(bytes));
    }

    if (const auto images = metadata.images_offset()) {
        note(map, MAP_ENTRY_ASSOCIATED_IMAGES, images.__offset, array_span(images));
        for (uint32_t i = 0; i < images.count(); ++i)
            if (const auto bytes = images.entry(i).bytes_offset())
                note(map, MAP_ENTRY_ASSOCIATED_IMAGE_BYTES, bytes.__offset,
                     b::IMAGE_BYTES::header_size + bytes.title_size() + bytes.image_size());
    }

    if (const auto profile = metadata.icc_color_offset())
        note(map, MAP_ENTRY_ICC_PROFILE, profile.__offset, blob_span(profile));

    if (const auto clinical = metadata.clinical_offset())
        note(map, MAP_ENTRY_CLINICAL_METADATA, clinical.__offset, blob_span(clinical));

    if (const auto annotations = metadata.annotations_offset()) {
        note(map, MAP_ENTRY_ANNOTATIONS, annotations.__offset, array_span(annotations));
        for (uint32_t i = 0; i < annotations.count(); ++i)
            if (const auto bytes = annotations.entry(i).bytes_offset())
                note(map, MAP_ENTRY_ANNOTATION_BYTES, bytes.__offset, blob_span(bytes));
        if (const auto sizes = annotations.group_sizes_offset())
            note(map, MAP_ENTRY_ANNOTATION_GROUP_SIZES, sizes.__offset, array_span(sizes));
        if (const auto blob = annotations.group_bytes_offset())
            note(map, MAP_ENTRY_ANNOTATION_GROUP_BYTES, blob.__offset, blob_span(blob));
    }

    return map;
}

// MARK: - Recovery

namespace {

/// Every block type, by tag, with the map entry it becomes. Derived from the
/// generated enumeration rather than a second literal table: the tag values
/// live in IFE_Constants.hpp and are stated once.
Abstraction::MapEntryType entry_for(k::RecoveryCodes __tag) noexcept {
    using namespace Abstraction;
    switch (__tag) {
        case k::RecoveryCodes::RECOVER_FILE_HEADER:            return MAP_ENTRY_FILE_HEADER;
        case k::RecoveryCodes::RECOVER_TILE_TABLE:             return MAP_ENTRY_TILE_TABLE;
        case k::RecoveryCodes::RECOVER_CIPHER:                 return MAP_ENTRY_CIPHER;
        case k::RecoveryCodes::RECOVER_METADATA:               return MAP_ENTRY_METADATA;
        case k::RecoveryCodes::RECOVER_ATTRIBUTES:             return MAP_ENTRY_ATTRIBUTES;
        case k::RecoveryCodes::RECOVER_LAYER_EXTENTS:          return MAP_ENTRY_LAYER_EXTENTS;
        case k::RecoveryCodes::RECOVER_TILE_OFFSETS:           return MAP_ENTRY_TILE_OFFSETS;
        case k::RecoveryCodes::RECOVER_ATTRIBUTE_SIZES:        return MAP_ENTRY_ATTRIBUTE_SIZES;
        case k::RecoveryCodes::RECOVER_ATTRIBUTE_BYTES:        return MAP_ENTRY_ATTRIBUTES_BYTES;
        case k::RecoveryCodes::RECOVER_IMAGES:                 return MAP_ENTRY_ASSOCIATED_IMAGES;
        case k::RecoveryCodes::RECOVER_IMAGE_BYTES:            return MAP_ENTRY_ASSOCIATED_IMAGE_BYTES;
        case k::RecoveryCodes::RECOVER_ICC_PROFILE:            return MAP_ENTRY_ICC_PROFILE;
        case k::RecoveryCodes::RECOVER_ANNOTATIONS:            return MAP_ENTRY_ANNOTATIONS;
        case k::RecoveryCodes::RECOVER_ANNOTATION_BYTES:       return MAP_ENTRY_ANNOTATION_BYTES;
        case k::RecoveryCodes::RECOVER_ANNOTATION_GROUP_SIZES: return MAP_ENTRY_ANNOTATION_GROUP_SIZES;
        case k::RecoveryCodes::RECOVER_ANNOTATION_GROUP_BYTES: return MAP_ENTRY_ANNOTATION_GROUP_BYTES;
        case k::RecoveryCodes::RECOVER_CLINICAL_METADATA:      return MAP_ENTRY_CLINICAL_METADATA;
        case k::RecoveryCodes::RECOVER_UNDEFINED:              break;
    }
    return MAP_ENTRY_UNDEFINED;
}

}  // namespace

Abstraction::FileMap recover_file_structure(BYTE* const __base, size_t __size) {
    using namespace Abstraction;
    FileMap map;
    map.file_size = __size;

    // A block's signature is its VALIDATION field: a u64 holding the block's
    // own offset, immediately followed by a u16 recovery tag. Scanning for
    // "a u64 equal to where it was found" is what makes VALIDATION worth its
    // eight bytes, and the shared 0x55 high byte on every tag is what keeps
    // the false-positive rate of the second test negligible.
    constexpr Size SIGNATURE = 8 + 2;

    // A tile frame has no tag: what identifies it is that its VALIDATION is
    // forty bits rather than sixty-four, which no other structure in the
    // format does. So the same pass tests both widths at each position.
    constexpr Size FRAME_SIGNATURE = 5;
    constexpr Size SMALLEST = SIGNATURE < FRAME_SIGNATURE ? SIGNATURE : FRAME_SIGNATURE;
    if (__size < SMALLEST) return map;

    // The scan has no file header to read a version from -- that header may be
    // the very thing that was lost -- so frames are read at the version this
    // build writes. Reading a frame from a *later* file still works: its
    // fields are laid out backward from the stream, so the ones this build
    // knows sit where they have always sat and the rest lie further back.
    for (Offset at = 0; at < __size; ++at) {
        if (at + SIGNATURE <= __size && ::IFE::load<uint64_t>(__base + at) == at) {
            const auto tag = static_cast<k::RecoveryCodes>(::IFE::load<uint16_t>(__base + at + 8));
            if (const MapEntryType type = entry_for(tag); type != MAP_ENTRY_UNDEFINED) {
                // Size is unknown without trusting fields the corruption may
                // have reached, so record the header only. A caller builds the
                // handle for the type and asks it, having validated it first.
                note(map, type, at, 0);
                continue;
            }
        }

        if (at + FRAME_SIGNATURE > __size) continue;
        if (::IFE::load_u40(__base + at) != at) continue;

        // The frame must fit behind the stream it precedes; a match too near
        // the start of file is a coincidence, not a frame.
        const Offset stream_at = at + FRAME_SIGNATURE;
        const b::TILE_PIXEL_DATA frame{__base, stream_at, __size, b::VERSION_WRITTEN};
        if (!frame.validate()) continue;

        // Recorded with the stream's extent left at zero, as every other type
        // here is. The frame does not carry a length and deliberately does not
        // -- how far a compressed stream runs is a question its codec answers,
        // and this layer knows nothing about codecs. What the frame supplies
        // is TILE_INDEX, which no amount of reading the stream can recover,
        // because streams may be written in any order.
        note(map, MAP_ENTRY_TILE_FRAME, stream_at - b::TILE_PIXEL_DATA::header_size,
             b::TILE_PIXEL_DATA::header_size);
        note(map, MAP_ENTRY_TILE_DATA, stream_at, 0);
    }

    return map;
}

}  // namespace IrisCodec
