/**
 * @file IrisFileExtension.hpp
 * @author Ryan Landvater
 * @brief The semantic layer: the public API, built on the generated block handles.
 * @date 2025-03-04
 *
 * @copyright Copyright (c) 2025 Ryan Landvater
 *
 * The part of the layer that is *not* generated, because it
 * encodes intent rather than layout: what to lift into RAM and what to leave
 * on disk, the order a file map is walked, what a recovery scan looks for.
 * Everything below it (offsets, widths, validation, block navigation) comes
 * from the spec JSON through generated_source/.
 *
 * The `Iris::File::Abstraction` structs below are the data model every consumer
 * reads — a change to one is a change to all of them.
 *
 * The entry points carry the house API shape: snake_case names, a single
 * `FileAccessInfo` argument (mapped pointer + size), and `Result` returns.
 * Iris-Codec consumes this surface directly.
 */

#ifndef IRIS_FILE_EXTENSION_HPP
#define IRIS_FILE_EXTENSION_HPP


#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// The IFE_EXPORT symbol-visibility scheme. Three states, not two: building
// the shared library exports; consuming that shared library imports;
// everything else — a static archive, an object library, a translation unit
// compiled straight into an executable — does neither and needs no attribute.
// CMake sets IFE_EXPORT_API=true when building the library (see
// CMakeLists.txt); consumers of a separately built shared library set
// IFE_IMPORT_API=true.
//
// This lives here rather than in a header of its own: this is the only
// header with exported symbols. The generated layer never exports (decision D
// — the block layer is deliberately out of the shared library), so the
// visibility scheme needs exactly one home.
#ifndef IFE_EXPORT_API
#define IFE_EXPORT_API      false
#endif
// Set this only when linking against a separately built IFE shared library.
#ifndef IFE_IMPORT_API
#define IFE_IMPORT_API      false
#endif

#ifndef IFE_EXPORT
    #if IFE_EXPORT_API
        #if defined(_MSC_VER)
        #define IFE_EXPORT  __declspec(dllexport)
        #else
        #define IFE_EXPORT  __attribute__ ((visibility ("default")))
        #endif
    #elif IFE_IMPORT_API
        #if defined(_MSC_VER)
        #define IFE_EXPORT  __declspec(dllimport)
        #else
        #define IFE_EXPORT
        #endif
    #else
        // Static or object linkage: no attribute. Default visibility is
        // hidden by CXX_VISIBILITY_PRESET (see CMakeLists).
        #define IFE_EXPORT
    #endif
#endif

#include "IrisTypes.hpp"
#include "IrisCodecTypes.hpp"

#include "IFE_Blocks.hpp"
// Iris::File::Window is the Emscripten windowed fetch (remote file ranges over
// HTTP). Native builds never use it; gating keeps it out of native
// consumers' translation units and out of the native install.
#ifdef __EMSCRIPTEN__
#include "IFE_Window.hpp"
#endif

// The hierarchy is the namespace: `Iris::` is the core (Iris-Headers — Result,
// Buffer, Memory, …), `Iris::File::` is this repository, and a codec builds on
// both. Nesting is why this layer sees `Iris::` names without a using-directive.
namespace Iris::File {
// IFE also composes the Iris-Headers vocabulary that still lives in its
// `IrisCodec` namespace — `Encoding`, `Metadata`, `AssociatedImageInfo`,
// `MetadataType`, … (IrisCodecTypes.hpp). Those types describe what is in the
// file, and IFE depends on Iris-Headers, not on the Iris-Codec repository; the
// namespace name is historical. Narrow this to explicit `using IrisCodec::X;`
// declarations if the breadth is ever a problem.
using namespace IrisCodec;

namespace Abstraction {
struct File;
}  // namespace Abstraction

// MARK: - ENTRY METHODS

/// Access to a mapped Iris file: the identity every file-level operation takes.
///
/// The file must be mapped read-only; the abstraction never writes through it.
/// A single argument keeps the entry points Vulkan-shaped — the receiver is
/// the struct — so a future access option (a Window for remote ranges, a
/// write intent) extends the struct, never the signature.
struct IFE_EXPORT FileAccessInfo {
    const BYTE* file_ptr  = nullptr;   ///< Read-only mapping of the file.
    Size        file_size = 0;         ///< Bytes available at file_ptr.
};

/// Quick check that a file opens with an Iris header: the magic number, the
/// recovery tag, and that the header fits within the mapping. This does NOT
/// validate — no offset is followed and no block below the header is read.
/// Use validate_file_structure for that.
///
/// IRIS_SUCCESS means the header matches. Anything else is a failure Result
/// carrying a message, which means rejecting a file costs a formatted string:
/// worth knowing when the caller is sorting a directory rather than opening a
/// slide it already expects to be one.
Result               IFE_EXPORT is_iris_codec_file      (const FileAccessInfo&) noexcept;

/**
 * @brief Performs deep file validation checks to ensure stuctural offsets are valid. This does NOT perform
 * specification validations.
 *
 * This performs a tree validation of objects and sub-objects to ensure their offsets properly.
 */
Result               IFE_EXPORT validate_file_structure (const FileAccessInfo&) noexcept;

/**
 * @brief Abstract the Iris file structure into memory for quick data access.
 *
 * Validates what it reads, and never repairs. Every block it follows must pass
 * validate() -- both witnesses agree, and every length the block declares fits
 * the file -- or this throws std::runtime_error naming the block and the check
 * that failed. A structurally damaged file is therefore refused here, not
 * opened. To reopen one, run the recovery engine (being rebuilt against
 * FastFHIR's census design) and call this again. validate_file_structure()
 * checks the whole graph without building anything; this checks the blocks it
 * lifts, as it lifts them.
 *
 * Until RC-10.1 this read the bytes it was pointed at without checking them:
 * one flipped bit in a length field read a gigabyte past an 832-byte file.
 *
 * This is a convenience function that maps the entire file structure into memory using the below
 * defined obejcts within the Iris::File::Abstraction namespace. These objects will allow quick lookup
 * of data. Please note: Abstractions will lift object parameters but not object data (for example,
 * if an image is abstracted, the encoding algorithm (JPEG/PNG/AVIF), width, height, byte offset location,
 * and number of bytes will be lifted; however the actual image bytes will remain untouched and must be
 * separately read. This keeps the abstraction layer quick but removes memory bloat.
 */
// START HERE: THIS IS THE MAIN ENTRY FUNCTION TO THE FILE
Abstraction::File    IFE_EXPORT abstract_file_structure (const FileAccessInfo&);

// MARK: - READ (PARSER)

class Parser_t;   ///< the read BODY — defined in "IFE_Parser.hpp"

/**
 * @brief The public READ handle for a mapped Iris file.
 *
 * Derives from `std::shared_ptr<Parser_t>`, so it IS the pointer it stands for:
 * copy it freely, pass it by value, test it with `if (parser)`. The `.` methods
 * are the read API a consumer uses; the body (`Parser_t`) is reached with `->`
 * when you include "IFE_Parser.hpp" — where the modification surface
 * (generate_file_map) lives. This mirrors the write handle `Builder` beside it
 * and FastFHIR's own `Parser` / `Builder` pair.
 *
 * Two ways to bind one. `Parser::open(path)` maps the file read-only and OWNS
 * the mapping: it stays mapped for as long as any copy of the handle lives, so
 * a reader that keeps the Parser keeps its bytes. `Parser(FileAccessInfo)`
 * BORROWS a mapping the caller owns, exactly as `FileAccessInfo` documents.
 * Binding is nearly free — nothing is lifted until the abstraction is asked
 * for, and then it is lifted once.
 *
 * THE SPLIT. A parser owns access to the file: the mapping, validation, the
 * lifted structure, and bounds-checked byte spans. It never decodes a stream;
 * that is the application's (a codec's) job.
 */
class Parser : public std::shared_ptr<Parser_t>
{
    using Base = std::shared_ptr<Parser_t>;
public:
    using Base::Base;      // every shared_ptr constructor
    using Base::reset;     // keep reset() beside the body's reset forwarders

    Parser() noexcept = default;
    Parser(std::nullptr_t) noexcept {}
    Parser(std::shared_ptr<Parser_t> body) noexcept : Base(std::move(body)) {}

    /// Bind to a mapping the caller owns (borrowed). Defined in the cpp (it
    /// needs the complete body).
    explicit Parser(const FileAccessInfo& info);

    /// Map @p path read-only and bind to it; the parser owns the mapping. The
    /// file is not validated here — `validate_file_structure()` does that, and
    /// `abstraction()` checks what it lifts. @throws std::runtime_error if the
    /// file cannot be mapped (missing, unreadable, or empty).
    static Parser open(const std::filesystem::path& path);

    /// Quick header check (magic, recovery tag, header fits). Does not validate.
    Result is_iris_codec_file() const noexcept;
    /// Deep structural validation of every offset in the graph.
    Result validate_file_structure() const noexcept;
    /// Lift the file structure into memory for quick data access (a fresh
    /// copy on each call; `abstraction()` lifts once and keeps it).
    Abstraction::File abstract_file_structure() const;

    /// The lifted structure, lifted once on first use and kept. Thread-safe.
    /// @throws std::runtime_error as `abstract_file_structure` does, on every
    ///         call, if the file is structurally damaged.
    const Abstraction::File& abstraction() const;

    /// The compressed stream of one tile, zero-copy. Empty for a NULL_TILE
    /// entry — "no tile at this grid position", not an error; a real tile is
    /// never empty. @throws std::out_of_range for a bad layer or tile.
    std::span<const BYTE> tile(uint32_t layer, uint32_t tile) const;

    /// How many focal planes the tile's stream carries: 1 on a single-plane
    /// layer, 0 for a NULL_TILE entry, otherwise the count its tile frame
    /// records (every stream of a Z-stacked layer is framed; the abstraction
    /// has already refused a file where one is not).
    /// @throws std::out_of_range for a bad layer or tile.
    uint16_t tile_planes(uint32_t layer, uint32_t tile) const;

    /// The compressed stream of one associated image, zero-copy.
    /// @throws std::out_of_range if no image carries @p label.
    std::span<const BYTE> image(const std::string& label) const;

    /// The borrowed mapping this parser reads.
    const FileAccessInfo& info() const noexcept;
    /// Pointer to the first mapped byte.
    const BYTE* data() const noexcept;
    /// Bytes available at @ref data.
    Size size() const noexcept;
};

// MARK: - WRITE (BUILDER)

/// Parameters for creating a writer over a fresh arena.
struct IFE_EXPORT BuilderCreateInfo {
    /// Reserved address range: sparse, so only touched pages cost RAM or disk,
    /// and never remapped — the base must not move under lock-free writers — so
    /// exhausting it is terminal. A caller writing a larger file passes more.
    /// Tile streams cannot be addressed past 2^40 (the u40 tile OFFSET), so a
    /// reservation beyond that buys nothing for tiles.
    Size                  capacity = Size{8} << 30;   // 8 GiB
    /// Where to write, created if absent. The caller chooses it — naming and
    /// moving files is the application's job; the builder writes where it is
    /// told and leaves the file complete and closed at `finalize`. Empty selects
    /// an anonymous arena (for a stream never destined for disk).
    std::filesystem::path filepath = {};
    /// Precede each tile stream with a tile frame (11 bytes: its global tile
    /// index and plane count, laid out backward from the stream's first byte).
    /// Optional on a single-plane layer, and encouraged — the frame is what
    /// lets recovery put a stream back in the right place — but it costs bytes.
    /// Ignored for a Z-stacked layer, where every stream is framed regardless:
    /// the frame is the only place the format records a tile's plane count.
    bool                  tile_frames = true;
};

/// The tile pyramid, declared once before any tile is appended. The same shape
/// `Abstraction::TileTable` reads back, minus the tile placements the builder
/// records itself.
struct IFE_EXPORT BuilderTileTableInfo {
    Encoding              encoding   = TILE_ENCODING_UNDEFINED;
    Format                format     = FORMAT_UNDEFINED;
    /// Width, height, and per layer the tile grid and scale. Layer 0 is the
    /// lowest-resolution layer.
    Extent                extent;
    /// Per layer, the greatest number of focal planes any one tile carries.
    /// Empty, 0 and 1 all mean single-plane; more than 1 makes the layer
    /// Z-stacked, and every stream of it is then framed.
    std::vector<uint16_t> planes;
    /// Edge length in pixels of the slide's square tiles.
    uint16_t              tileLength = 256;
};

/// What `Builder::finalize` writes into the FILE_HEADER: the two roots the
/// caller placed, and the revision. The builder fills FILE_SIZE itself.
struct IFE_EXPORT BuilderFinalizeInfo {
    /// The TILE_TABLE block the caller placed (block tier: `append`).
    Offset   tileTable = ::Iris::File::constants::NULL_OFFSET;
    /// The METADATA block the caller placed (block tier: `append` or `fill`).
    Offset   metadata  = ::Iris::File::constants::NULL_OFFSET;
    /// FILE_REVISION.
    uint32_t revision  = 0;
};

class Builder_t;   ///< the write BODY — defined in "IFE_Builder.hpp"

/**
 * @brief The public WRITE handle for an Iris file.
 *
 * Derives from `std::shared_ptr<Builder_t>`, so it IS the pointer it stands
 * for: copy it freely, pass it by value, test it with `if (builder)`. The `.`
 * methods are the API a producer uses; the body (`Builder_t`) is reached with
 * `->` when you include "IFE_Builder.hpp", where the block-level tier lives
 * (`claim`, `append(XxxCreateInfo)`, `seal`). This mirrors FastFHIR's
 * `Builder` / `Builder_t` and the read handle `Parser` beside it.
 *
 * THE SPLIT. A builder controls how the stream is mutated: it claims space at
 * the head and fills it, records where each tile landed, frames the tiles, and
 * checks every tile was accounted for. It does NOT choose the layout. Every
 * `append_*` lands at the head when it is called, so the order of the calls is
 * the order on disk, and that order is the caller's to decide — as it is for a
 * FastFHIR Builder. It starts no threads and schedules no work — `append_tile`
 * is safe to call from many threads at once, but which thread encodes which
 * tile is the caller's business. It does not choose or move files: the caller
 * names the file and moves it once `finalize` returns.
 *
 * Usage: `create` → `set_tile_table` → `append_tile` / `append_null_tile` for
 * every tile (any order, any threads) and `append_image` per associated image
 * → compose the structure in the order you choose through the block tier
 * (`builder->append` / `claim` + `fill`), building the tile offsets from
 * `tile_offsets()` and the images array from `image_entries()` → `finalize`
 * with the tile table and metadata offsets.
 */
class Builder : public std::shared_ptr<Builder_t>
{
    using Base = std::shared_ptr<Builder_t>;
public:
    using Base::Base;      // every shared_ptr constructor
    using Base::reset;     // keep reset() beside the body's reset forwarders

    Builder() noexcept = default;
    Builder(std::nullptr_t) noexcept {}
    Builder(std::shared_ptr<Builder_t> body) noexcept : Base(std::move(body)) {}

    /** @brief Create a writer over a fresh arena. Throws if the arena cannot
     *         be mapped. */
    static Builder create(const BuilderCreateInfo& info);

    /** @brief Declare the tile pyramid. Once, before any tile is appended.
     *  @throws std::logic_error if called twice or after `finalize`;
     *          std::invalid_argument for no layers, or `planes` of the wrong
     *          length. */
    void set_tile_table(const BuilderTileTableInfo& info) const;

    /**
     * @brief Write one compressed tile stream and record where it landed.
     *
     * Thread-safe. @p layer and @p tile address the grid (row-major within the
     * layer); @p z_planes is the number of focal planes this stream carries (0
     * or 1 on a single-plane layer, at most the layer's `planes` on a
     * Z-stacked one). Returns the stream's offset — the anchor the tile
     * offsets entry names; a frame, when written, sits just before it.
     *
     * @throws std::logic_error before `set_tile_table` or after `finalize`,
     *         or if the tile was already appended; std::out_of_range for a bad
     *         layer or tile;
     *         std::invalid_argument for an empty stream, one of 16 MiB or
     *         more, or a plane count the layer does not allow.
     */
    Offset append_tile(uint32_t layer, uint32_t tile, const BYTE* data, Size size,
                       uint16_t z_planes = 0) const;

    /** @brief Record that there is no tile at this grid position (NULL_TILE).
     *         Thread-safe; writes no bytes. Same errors as `append_tile`. */
    void append_null_tile(uint32_t layer, uint32_t tile) const;

    /** @brief Write one associated image (label + compressed stream) and record
     *         its entry for `image_entries`. Returns the IMAGE_BYTES block's
     *         offset.
     *  @throws std::invalid_argument for an empty stream, a label longer than
     *          65535 bytes, or a label already appended. */
    Offset append_image(const AssociatedImageInfo& info, const BYTE* data, Size size) const;

    // The structure blocks are the caller's to compose: the Builder placed the
    // tiles and the images, so it reports what it placed, and the caller names
    // each block (TileOffsetsCreateInfo, LayerExtentsCreateInfo, …) and its
    // order. These two are that report — write state, not a read of the file.

    /** @brief Every tile's placement, in global tile-index order (layer 0's
     *         tiles first), as `append_tile` / `append_null_tile` recorded it.
     *  @throws std::logic_error before `set_tile_table`, or for a tile that was
     *         never appended. Call once every append has joined. */
    std::vector<blocks::TileOffsetEntry> tile_offsets() const;

    /** @brief The associated-image entries, in append order, as `append_image`
     *         recorded them — the source a caller composes `ImagesCreateInfo`
     *         from. */
    std::vector<blocks::ImageEntry> image_entries() const;

    /** @brief A Parser over the builder's own mapping, so a read is never
     *         re-implemented here (FastFHIR's `Builder_t::query()`). Meaningful
     *         once the stream has a header: a mounted stream, or after
     *         `finalize`. */
    Parser query() const;

    /**
     * @brief Seal the file: write the FILE_HEADER naming @p info's tile table
     *        and metadata, with FILE_SIZE the committed head.
     *
     * A file-backed builder releases its mapping and truncates the file to its
     * written size, leaving it closed for the caller to move. After this the
     * builder accepts no more writes. Call it once every append has joined.
     * @throws std::invalid_argument, before any write, if either root is
     *         NULL_OFFSET.
     */
    void finalize(const BuilderFinalizeInfo& info) const;

    /** @brief The committed write head — the offset the next block lands at. */
    ::Iris::File::Offset head() const noexcept;

    /** @brief The arena's reserved extent in bytes. */
    Size capacity() const noexcept;

    /** @brief Whether `finalize` / `seal` has run; a finalized builder accepts
     *         no more writes. */
    [[nodiscard]] bool is_finalized() const noexcept;
};

// generate_file_map and its file-map value types (FileMap, MapEntryType, Gap,
// ...) live in "IFE_Recovery.hpp", with the recovery engine. A consumer that
// only READS a slide includes this header alone and never sees them.

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
    Offset offset = ::Iris::File::constants::NULL_OFFSET;
};

/// Compressed tile data byte offset and size within the slide file.
struct IFE_EXPORT TileEntry {
    Offset   offset = ::Iris::File::constants::NULL_OFFSET;
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
    uint16_t tileLength = 256;
};

/// Abstraction of non-tile and named associated images within the slide file.
struct IFE_EXPORT AssociatedImage {
    using Info = AssociatedImageInfo;
    Offset offset   = ::Iris::File::constants::NULL_OFFSET;
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
    Offset   offset    = ::Iris::File::constants::NULL_OFFSET;
    Size     byteSize  = 0;
    Type     type      = ANNOTATION_UNDEFINED;
    float    xLocation = 0.f;
    float    yLocation = 0.f;
    float    xSize     = 0.f;
    float    ySize     = 0.f;
    uint32_t width     = 0;
    uint32_t height    = 0;
    uint32_t parent    = 0;

    // MSVC's <unordered_map> instantiates pair::operator== as part of the
    // Annotations map's class instantiation (GCC/Clang defer it until the
    // map is actually compared), so the value type must be equality-
    // comparable even when the map is never compared. Defaulted: every
    // member is a scalar with the natural equality.
    bool operator==(const Annotation&) const = default;
};

struct IFE_EXPORT AnnotationGroup {
    Offset   offset = ::Iris::File::constants::NULL_OFFSET;
    uint32_t number = 0;
    Size     byteSize() { return number * 3; }
};

struct IFE_EXPORT Annotations : public std::unordered_map<Annotation::Identifier, Annotation> {
    using Groups = std::unordered_map<std::string, AnnotationGroup>;
    Groups groups;
};

struct AttributeNode;
/// A complete attribute structure: what one ATTRIBUTES block carries.
using AttributeSet = std::vector<AttributeNode>;

/// One attribute, with the value's structure preserved.
///
/// IrisCodec::Metadata::attributes is a flat map of string to string, so a
/// value that is a sequence of nested attribute sets has nowhere to go in it.
/// The alternative to carrying the tree here would be flattening it back into
/// path-shaped keys — which is the convention the nested wire format exists to
/// replace, so reintroducing it at the abstraction boundary would give the
/// change away for nothing.
struct IFE_EXPORT AttributeNode {
    /// The attribute key. ASCII, or a four-byte DICOM tag.
    std::string   key;
    /// The value's text, when this is not a sequence.
    std::u8string value;
    /// One entry per sequence item, each item a complete attribute set — the
    /// shape of a DICOM sequence, whose items are each a data set.
    std::vector<AttributeSet> items;
    /// Whether the value is a sequence. Stated rather than inferred from
    /// `items` being empty: a sequence of no items is legal and is not the
    /// same thing as a text value of length zero.
    bool          nested = false;
};

/// In-memory abstraction of the Iris file structure.
struct IFE_EXPORT File {
    Header           header;
    TileTable        tileTable;
    AssociatedImages images;
    Annotations      annotations;
    Metadata         metadata;
    /// The attribute structure as the file carries it, nesting included.
    ///
    /// The complete picture; Metadata::attributes carries the top-level text
    /// values as well, flat, for callers that want the map they always had.
    /// Empty when the file encodes no attributes at all.
    ///
    /// Held on File rather than on Metadata beside the flat map for the same
    /// reason clinicalOffset is: IrisCodec::Metadata is defined in
    /// Iris-Headers rather than in this repository. Move it there when that
    /// header gains the field.
    AttributeSet     attributeTree;
    /// Byte range of the clinical metadata stream, NULL_OFFSET when absent.
    ///
    /// A range rather than a copy, unlike Metadata::ICC_profile. A colour
    /// profile is a few kilobytes; this is a whole resource graph and can be
    /// megabytes, and lifting payloads into the abstraction is the one thing
    /// this abstraction exists not to do. Read it with Iris::File::Window, or hand
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
    Offset           clinicalOffset = ::Iris::File::constants::NULL_OFFSET;
    Size             clinicalSize   = 0;
    /// Which parser the clinical stream needs. Declared by the file rather
    /// than sniffed from the bytes; undefined when no stream is present.
    /// A `clinical_encodings` value, held as its underlying type rather than
    /// as Iris::File::constants::ClinicalEncodings. Naming the generated enum here
    /// would put it in the exported ABI -- and with it every
    /// std::optional<ClinicalEncodings> instantiation the runtime makes --
    /// which decision 4.0-D keeps out. Zero is CLINICAL_UNDEFINED.
    uint8_t          clinicalEncoding = 0;
    /// Microns between adjacent focal planes of a Z-stacked tile; zero when
    /// the slide is not Z-stacked or the spacing was not recorded.
    float            micronsPerPlane = 0.f;
};

// The file-map value types (MapEntryType, GapClass/Gap, FileMapEntry,
// FileMap, ProducerFailure(Kind)) and generate_file_map live in
// "IFE_Recovery.hpp", with the recovery engine. This header is the READ
// surface only: a consumer that opens a slide includes it alone.

}  // namespace Abstraction
}  // namespace Iris::File

// =====================================================================
// GLOBAL C-STYLE ALIASES — the READ tier
// =====================================================================
// Inside the namespace every type carries its plain C++ name; these
// IFE_-prefixed names are that type's *global* spelling, for a consumer that
// does not want to qualify with Iris::File:: on every line. The IFE_ prefix is
// the namespace spelled out — the same convention FastFHIR's FF_ aliases use
// (../FastFHIR/include/FastFHIR.hpp). Alias only: nothing here renames a
// namespaced symbol, so existing consumers (Iris-Codec) are unaffected.
//
// The file-map aliases (FileMap, MapEntryType, Gap, ...) live in
// "IFE_Recovery.hpp" beside those types.
//
// Freeze: tests/ife_api_contract_tests.cpp asserts each of these resolves to
// its namespaced type, so a rename on either side fails that build.
using IFE_FileAccessInfo      = Iris::File::FileAccessInfo;
using IFE_File                = Iris::File::Abstraction::File;
using IFE_TileTable           = Iris::File::Abstraction::TileTable;
using IFE_TileEntry           = Iris::File::Abstraction::TileEntry;
using IFE_Header              = Iris::File::Abstraction::Header;
using IFE_AssociatedImage     = Iris::File::Abstraction::AssociatedImage;
using IFE_AssociatedImages    = Iris::File::Abstraction::AssociatedImages;
using IFE_Annotation          = Iris::File::Abstraction::Annotation;
using IFE_Annotations         = Iris::File::Abstraction::Annotations;
using IFE_AttributeNode       = Iris::File::Abstraction::AttributeNode;
using IFE_AttributeSet        = Iris::File::Abstraction::AttributeSet;

#endif  // IRIS_FILE_EXTENSION_HPP
