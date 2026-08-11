/**
 * @file IFE_Runtime.hpp
 * @brief The semantic layer: the public API, built on the generated block handles.
 * @copyright Iris Developers, 2025-2026
 *
 * The part of the layer that is *not* generated, because it
 * encodes intent rather than layout: what to lift into RAM and what to leave
 * on disk, the order a file map is walked, what a recovery scan looks for.
 * Everything below it (offsets, widths, validation, block navigation) comes
 * from the spec JSON through generated_source/.
 *
 * This is a **port, not a redesign**. The entry-point names, signatures and
 * doc-comments are v1's, and the `IrisCodec::Abstraction` structs are
 * structurally identical to the ones in src/IrisCodecExtension.hpp, because
 * they are the contract Iris-Codec consumes: changing them would turn the
 * legacy-layer cutover from a re-point into a rewrite.
 *
 * **This header and IrisCodecExtension.hpp are mutually exclusive.** Both
 * define IrisCodec::Abstraction, deliberately — that is what lets a consumer
 * switch by changing one include line — so including both is an error rather
 * than a redefinition diagnostic thirty lines deep in a template.
 */

#ifndef IFE_Runtime_hpp
#define IFE_Runtime_hpp

#ifdef IrisCodecExtension_hpp
#error "IFE_Runtime.hpp and IrisCodecExtension.hpp both define IrisCodec::Abstraction. \
Include one or the other: IrisCodecExtension.hpp is the hand-written layer, retired; \
IFE_Runtime.hpp is the generated-layer successor."
#endif

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "IFE_Export.hpp"
#include "IrisTypes.hpp"
#include "IrisCodecTypes.hpp"

#include "IFE_Blocks.hpp"
#include "IFE_Window.hpp"

namespace IrisCodec {
using namespace Iris;

namespace Abstraction {
struct File;
struct FileMap;
}  // namespace Abstraction

// MARK: - ENTRY METHODS

/// Perform quick check to see if this file header matches an Iris format. This does NOT validate.
bool IFE_EXPORT is_Iris_Codec_file(BYTE* const __mapped_file_ptr, size_t file_size);

/**
 * @brief Performs deep file validation checks to ensure stuctural offsets are valid. This does NOT perform
 * specification validations.
 *
 * This performs a tree validation of objects and sub-objects to ensure their offsets properly.
 */
Result IFE_EXPORT validate_file_structure(BYTE* const __mapped_file_ptr, size_t file_size) noexcept;

/**
 * @brief Abstract the Iris file structure into memory for quick data access. This does NOT validate.
 *
 * This is a convenience function that maps the entire file structure into memory using the below
 * defined obejcts within the IrisCodec::Abstraction namespace. These objects will allow quick lookup
 * of data. Please note: Abstractions will lift object parameters but not object data (for example,
 * if an image is abstracted, the encoding algorithm (JPEG/PNG/AVIF), width, height, byte offset location,
 * and number of bytes will be lifted; however the actual image bytes will remain untouched and must be
 * separately read. This keeps the abstraction layer quick but removes memory bloat.
 */
// START HERE: THIS IS THE MAIN ENTRY FUNCTION TO THE FILE
Abstraction::File IFE_EXPORT abstract_file_structure(BYTE* const __mapped_file_ptr, size_t file_size);

/**
 * @brief Generate a file map showing the offset locations of header and array blocks with their respective
 * types and sizes detailed. This is not a cheap method and does not need to be routinely done; only when
 * recovering or modifying a file.
 *
 * File mapping is an extremely valuable tool for performing file updates to avoid overwriting important data.
 * Fortunately it is very simple to do. Before writing, perform the \ref FileMap::upper_bound (Offset write_offset) method
 * to identify what data exists after your proposed write location. These data will need to be rewritten or,
 * alternatively, shifted and all references to them and their validations updated as well. For this reason, it's usually
 * easier to simply read them into memory and then rewrite them back to disk following the update.
 */
// ALWAYS CREATE A FILE MAP BEFORE PERFORMING AN UPDATE TO A FILE
Abstraction::FileMap IFE_EXPORT generate_file_map(BYTE* const __mapped_file_ptr, size_t file_size);

/**
 * @brief Recover the block structure of a damaged file by scanning for block signatures.
 *
 * New in the generated layer. Where generate_file_map walks the offset graph — and therefore
 * finds nothing below a corrupted pointer — this ignores the graph entirely and scans for the
 * two signatures a block can carry.
 *
 * Most blocks carry a u64 equal to their own offset followed by a u16 in the recovery-tag set;
 * the 0x55 high byte those tags share is what keeps that scan's false-positive rate negligible.
 * A tile frame carries no tag at all, and is instead identified by a *forty*-bit value equal to
 * its own position, which nothing else in the format writes. Frames are reported as
 * MAP_ENTRY_TILE_FRAME alongside the MAP_ENTRY_TILE_DATA stream each one describes.
 *
 * A frame supplies the part of a tile offsets entry that reading the slide cannot: its global
 * tile index. Position cannot supply it, because streams may be written in any order. The
 * stream's *length* is not in the frame and is not reported — that is a question its codec
 * answers, and this layer knows nothing about codecs. FileMapEntry carries no index field, so as
 * with every other type a caller builds the handle and asks it:
 *
 * ```cpp
 * case MAP_ENTRY_TILE_FRAME: {
 *     const auto stream_at = entry.offset + entry.size;   // the frame ends where the stream starts
 *     IFE::blocks::TILE_PIXEL_DATA frame {base, stream_at, file_size, version};
 *     if (frame.validate()) rebuilt[*frame.tile_index()] = stream_at;
 * }
 * ```
 *
 * The FILE_HEADER is not recoverable this way and is not reported: it is the one block with no
 * VALIDATION field, because it lives at byte 0 where that field could only ever store zero.
 */
Abstraction::FileMap IFE_EXPORT recover_file_structure(BYTE* const __mapped_file_ptr, size_t file_size);

// MARK: - FILE ABSTRACTIONS
// The file abstractions pull light-weight
// representations of the on-disk information
// such as critial offset locations and sizes
// of larger image or vector payloads
namespace Abstraction {

/// Extracted file header information.
struct IFE_EXPORT Header {
    Size     fileSize   = 0;
    uint32_t extVersion = 0;
    uint32_t revision   = 0;
};

/// RESERVED FOR FUTURE IRIS CODEC IMPLEMENTATION.
struct IFE_EXPORT Cipher {
    Offset offset = ::IFE::constants::NULL_OFFSET;
};

/// Compressed tile data byte offset and size within the slide file.
struct IFE_EXPORT TileEntry {
    Offset   offset = ::IFE::constants::NULL_OFFSET;
    uint32_t size   = 0;
};

/// Light-weight in-memory representation of the WSI file mapped tile data.
struct IFE_EXPORT TileTable {
    using Layer  = std::vector<TileEntry>;
    using Layers = std::vector<Layer>;
    /// Greatest number of focal (Z) planes any one tile of a layer carries,
    /// one element per `extent.layers` entry; a given tile may carry fewer,
    /// and its stream is what says how many. Always at least one: a file
    /// written before 1.1 stores no plane count and reads back as
    /// single-plane rather than as zero.
    ///
    /// Held here, parallel to extent.layers, only because Iris::LayerExtent
    /// is defined in Iris-Headers rather than in this repository -- it is the
    /// natural home, and reserves the field for it (IrisTypes.hpp `zPlanes`).
    /// Fold this in and delete the vector once that field exists.
    using Planes = std::vector<uint16_t>;
    Encoding encoding = TILE_ENCODING_UNDEFINED;
    Format   format   = FORMAT_UNDEFINED;
    Layers   layers;
    Extent   extent;
    Planes   planes;
    /// Edge length in pixels of this slide's square tiles; 256 unless the file
    /// says otherwise, including for every file written before 1.1.
    uint16_t tileSize = 256;
};

/// Abstraction of non-tile and named associated images within the slide file.
struct IFE_EXPORT AssociatedImage {
    using Info = AssociatedImageInfo;
    Offset offset   = ::IFE::constants::NULL_OFFSET;
    Size   byteSize = 0;
    Info   info;
};

/// Label-image dictionary for associated images.
using AssociatedImages = std::unordered_map<std::string, AssociatedImage>;

/// On-slide annotation, by 24-bit identifier.
struct IFE_EXPORT Annotation {
    using Identifier = Iris::Annotation::Identifier;
    static constexpr uint32_t NULL_ID = 16777215U;

    using Type = AnnotationTypes;
    Offset   offset    = ::IFE::constants::NULL_OFFSET;
    Size     byteSize  = 0;
    Type     type      = ANNOTATION_UNDEFINED;
    float    xLocation = 0.f;
    float    yLocation = 0.f;
    float    xSize     = 0.f;
    float    ySize     = 0.f;
    uint32_t width     = 0;
    uint32_t height    = 0;
    uint32_t parent    = 0;
};

struct IFE_EXPORT AnnotationGroup {
    Offset   offset = ::IFE::constants::NULL_OFFSET;
    uint32_t number = 0;
    Size     byteSize() { return number * 3; }
};

struct IFE_EXPORT Annotations : public std::unordered_map<Annotation::Identifier, Annotation> {
    using Groups = std::unordered_map<std::string, AnnotationGroup>;
    Groups groups;
};

/// In-memory abstraction of the Iris file structure.
struct IFE_EXPORT File {
    Header           header;
    TileTable        tileTable;
    AssociatedImages images;
    Annotations      annotations;
    Metadata         metadata;
    /// Byte range of the clinical metadata stream, NULL_OFFSET when absent.
    ///
    /// A range rather than a copy, unlike Metadata::ICC_profile. A colour
    /// profile is a few kilobytes; this is a whole resource graph and can be
    /// megabytes, and lifting payloads into the abstraction is the one thing
    /// this abstraction exists not to do. Read it with IFE::Window, or hand
    /// base + clinicalOffset straight to the reader for whatever format the
    /// stream's leading bytes identify.
    ///
    /// This is the only member here that carries identity. Laboratory metadata
    /// is key-value and lives in Metadata::attributes; de-identifying a slide
    /// drops this stream alone and leaves that untouched.
    ///
    /// Held on File rather than on Metadata beside ICC_profile only because
    /// IrisCodec::Metadata is defined in Iris-Headers rather than in this
    /// repository. Move it there when that header gains the field.
    Offset           clinicalOffset = ::IFE::constants::NULL_OFFSET;
    Size             clinicalSize   = 0;
    /// Which parser the clinical stream needs. Declared by the file rather
    /// than sniffed from the bytes; undefined when no stream is present.
    /// A `clinical_encodings` value, held as its underlying type rather than
    /// as IFE::constants::ClinicalEncodings. Naming the generated enum here
    /// would put it in the exported ABI -- and with it every
    /// std::optional<ClinicalEncodings> instantiation the runtime makes --
    /// which decision 4.0-D keeps out. Zero is CLINICAL_UNDEFINED.
    uint8_t          clinicalEncoding = 0;
    /// Microns between adjacent focal planes of a Z-stacked tile; zero when
    /// the slide is not Z-stacked or the spacing was not recorded.
    float            micronsPerPlane = 0.f;
};

/// Which kind of block a file-map entry describes.
enum MapEntryType {
    MAP_ENTRY_UNDEFINED = 0,
    MAP_ENTRY_FILE_HEADER,
    MAP_ENTRY_TILE_TABLE,
    MAP_ENTRY_CIPHER,
    MAP_ENTRY_METADATA,
    MAP_ENTRY_ATTRIBUTES,
    MAP_ENTRY_LAYER_EXTENTS,
    MAP_ENTRY_TILE_DATA,
    MAP_ENTRY_TILE_OFFSETS,
    MAP_ENTRY_ATTRIBUTE_SIZES,
    MAP_ENTRY_ATTRIBUTES_BYTES,
    MAP_ENTRY_ASSOCIATED_IMAGES,
    MAP_ENTRY_ASSOCIATED_IMAGE_BYTES,
    MAP_ENTRY_ICC_PROFILE,
    MAP_ENTRY_ANNOTATIONS,
    MAP_ENTRY_ANNOTATION_BYTES,
    MAP_ENTRY_ANNOTATION_GROUP_SIZES,
    MAP_ENTRY_ANNOTATION_GROUP_BYTES,
    MAP_ENTRY_CLINICAL_METADATA,
    MAP_ENTRY_TILE_FRAME,
};

/**
 * @brief A datablock within the IFE file structure system.
 *
 * **The one member that could not be carried over verbatim.** v1's entry held
 * a `Serialization::DATA_BLOCK` — a class that dies with the hand-written
 * layer — which a caller then `static_cast` to the concrete block type to read
 * it. There is no successor to cast to: a generated handle is constructed from
 * an offset, not downcast from a base. So the entry carries the offset and the
 * type, and a caller builds the handle it wants:
 *
 * ```cpp
 * case MAP_ENTRY_TILE_TABLE: {
 *     IFE::blocks::TILE_TABLE table {base, entry.offset, file_size, version};
 *     if (table.validate()) ... // read through the handle
 * }
 * ```
 *
 * Strictly more capable than the v1 form, which could only produce a block at
 * the version the map was built with, and the only place in this header where
 * a consumer's code changes at the cutover.
 */
struct IFE_EXPORT FileMapEntry {
    MapEntryType type   = MAP_ENTRY_UNDEFINED;
    Offset       offset = ::IFE::constants::NULL_OFFSET;
    Size         size   = 0;
};

struct IFE_EXPORT FileMap : public std::map<Offset, FileMapEntry> {
    Size file_size = 0;
};

}  // namespace Abstraction
}  // namespace IrisCodec

#endif  // IFE_Runtime_hpp
