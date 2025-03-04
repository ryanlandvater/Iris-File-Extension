/**
 * @file IrisCodecExtension.hpp
 * @author Ryan Landvater (RyanLandvater@gmail.com)
 * @brief  Iris Codec API Documentation.
 * @version 2025.1.0
 * @date 2024-01-11
 *
 * @copyright Copyright (c) 2022-25 Ryan Landvater
 * Version 0 was created by Ryan Landvater on 8/12/2022
 *
 * The .iris (Iris) File Extension
 * Part of the Iris Whole Slide Imaging Project
 *
 * Use of Iris Codec and the Iris File Extension (.iris) follows the
 * CC BY-ND 4.0 License outlined in the Iris Digital Slide Extension File Structure Techinical Specification
 * https://creativecommons.org/licenses/by-nd/4.0/
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the “Software”), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Use of this code constitutes your implicit agreement to these requirements.
 */


// Enable encoding validation checks to ensure
// the encoded slide adheres to the IFE specification.
// This setting will make your binary slightly larger
// but is highly recommended if encoding any aspect of a slide.
#define IrisCodecExtensionValidateEncoding 1

// Note: Decoding extension validation cannot be disabled.

// PLEASE NOTE (this is important):
// ** Errors that voilate the standard are thrown as uncaught exceptions.
// It is the programmers' imperative to catch these exceptions; resolving
// these runtime errors is outside the intended scope of this implementation.

#ifndef IrisCodecExtension_hpp
#define IrisCodecExtension_hpp
namespace IrisCodec {
using namespace Iris;
constexpr Offset   NULL_OFFSET          = UINT64_MAX;

// IRIS EXTENSION VERSION FOR WHICH THIS HEADER CORRESPONDS
constexpr uint16_t IRIS_EXTENSION_MAJOR = 1;
constexpr uint16_t IRIS_EXTENSION_MINOR = 0;

// Iris' Magic Number is ASCII for 'Iris' 49 72 69 73
#define MAGIC_BYTES 0x49726973

/// Perform quick check to see if this file header matches an Iris format. This does NOT validate.
bool is_Iris_Codec_file     (BYTE* const __mapped_file_ptr, size_t file_size);
/**
 * @brief Performs deep file validation checks to ensure stuctural offsets are valid. This does NOT perform
 * specification validations.
 *
 * This performs a tree validation of objects and sub-objects to ensure their offsets properly.
 */
void validate_file_structure (BYTE* const __mapped_file_ptr, size_t file_size);

// MARK: - FILE ABSTRACTIONS
// The file abstractions pull light-weight
// representations of the on-disk information
// such as critial offset locations and sizes
// of larger image or vector payloads
namespace Abstraction {
struct File;
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
File abstract_file_structure (BYTE* const __mapped_file_ptr, size_t file_size);

/**
 * @brief Extracted file header information
 *
 * The extracted version does not contain metadata
 * used to validate the file such as the magic number;
 * this was used internally already to produce the footer
 */
struct Header {
    Size            fileSize    = 0;
    uint32_t        extVersion  = 0;
    uint32_t        revision    = 0;
};
/**
 * @brief RESERVED FOR FUTURE IRIS CODEC IMPLEMENTATION
 *
 * This is related to ongoing research related to image
 * compression and is reserved for future use.
 */
struct Cipher {
    Offset          offset      = NULL_OFFSET;
};
/**
 * @brief Compressed tile data byte offset and size within
 * the slide file.
 *
 * For a mapped WSI file with the start of the file being
 * the pointer in v-address space (file-ptr):\n
 * Memcpy (file-ptr + TileEntry::offset, dst, TileEntry::size) will
 * read the compressed file byte stream into the memory
 * pointed to by dst.
 */
struct TileEntry {
    Offset          offset      = NULL_OFFSET;
    uint32_t        size        = 0;
};
/**
 * @brief Light-weight in-memory representation of the WSI
 * file mapped tile data.
 *
 * This will give all information necessary to decode the
 * WSI tiles into a renderable format.
 *
 * Cipher is reserved for future use.
 *
 * The extent (TileTable::Extent) is the Iris::Extent detailing the
 * pixel width / height of the level (0) / most zoomed out image
 * view as well as the layer extents in standard Iris tiles
 * (256x256 pixel tiles).
 *
 * The layers (TileTable::Layers) is an
 * array of layer arrays giving the byte-offset locations
 * of each tile of each layer relative to the beginning
 * of the whole file (ie byte 0 of the mapped file).
 */
struct TileTable {
    using Layer     = std::vector<TileEntry>;
    using Layers    = std::vector<Layer>;
    Encoding        encoding    = TILE_ENCODING_UNDEFINED;
    Format          format      = FORMAT_UNDEFINED;
    Layers          layers;
    Extent          extent;
};
/**
 * @brief Abstraction of non-tile and named associated
 * images within the slide file.
 *
 * This will extract all information needed to provide
 * the images to a rendering system without actually
 * reading the image data from disk and decompressing it.
 *
 */
struct Image {
    using           Encoding    = ImageEncoding;
    using           Orientation = ImageOrientation;
    std::string     title;
    Offset          offset      = NULL_OFFSET;
    Size            byteSize    = 0;
    uint32_t        width       = 0;
    uint32_t        height      = 0;
    Encoding        encoding    = IMAGE_ENCODING_UNDEFINED;
    Format          format      = Iris::FORMAT_UNDEFINED;
    Orientation     orientation = ORIENTATION_0;
};
/**
 * @brief Label-image dictionary for associated images
 */
using Images = std::unordered_map<std::string, Image>;
/**
 * @brief Annotation abstraction containing on-slide annotations by annotation
 * identifier (24-bit value) and annotation groups by group name (string)
 *
 */
struct Annotation {
    static constexpr
    uint32_t    NULL_ID     = 16777215U;
    
    using Type              = AnnotationTypes;
    Offset      offset      = NULL_OFFSET;
    Size        byteSize    = 0;
    Type        type        = ANNOTATION_UNDEFINED;
    float       xLocation   = 0.f;
    float       yLocation   = 0.f;
    float       xSize       = 0.f;
    float       ySize       = 0.f;
    uint32_t    width       = 0;
    uint32_t    height      = 0;
    uint32_t    parent      = 0;
};
using Annotations = std::unordered_map<uint32_t, Annotation>;
/**
 * @brief In-memory abstraction of the Iris file structure
 *
 * This is a low-overhead file abstraction that allows for
 * fast access to the underlying slide data.
 */
struct File {
    Header          header;
    TileTable       tileTable;
    Images          images;
    Annotations     annotations;
    Metadata        metadata;
};
}
// MARK: - IRIS CODEC EXTENSION SERIALIZATION TYPES
namespace Serialization {
using namespace Abstraction;
using MagicBytes                    = uint_least32_t;
// MARK: HEADER TYPES
struct FILE_HEADER;
struct TILE_TABLE;
struct METADATA;
struct ATTRIBUTES;

// MARK: ARRAY TYEPES
struct LAYER_EXTENTS;
struct TILE_OFFSETS;
struct ATTRIBUTES_SIZES;
struct ATTRIBUTES_BYTES;
struct IMAGE_ARRAY;
struct IMAGE_BYTES;
struct ICC_PROFILE;
struct ANNOTATION_ARRAY;
struct ANNOTATION_BYTES;
struct ANNOTATION_GROUPS;

/**
 * @brief Iris Codec statically definied offset values
 */
enum Offsets : uint_least64_t {
    NULL_OFFSET                     = UINT64_MAX,
};
/**
 * @brief Iris Codec Files contain methods to
 * heal corrupted metadata in the event of errors
 * occur in saving or outside of the scope of Iris
 * 
 */
enum RECOVERY : uint_least16_t {
    // In the event of recovery, we will search
    // for a byte offset that stores its own value
    // followed by one of these sequences.
    RECOVER_HEADER                  = 0x5501,
    RECOVER_TILE_TABLE              = 0x5502,
    RECOVER_CIPHER                  = 0x5503,
    RECOVER_METADATA                = 0x5504,
    RECOVER_ATTRIBUTES              = 0x5505,
    RECOVER_LAYER_EXTENTS           = 0x5506,
    RECOVER_TILE_OFFSETS            = 0x5507,
    RECOVER_ATTRIBUTES_SIZES        = 0x5508,
    RECOVER_ATTRIBUTES_BYTES        = 0x5509,
    RECOVER_ASSOCIATED_IMAGES       = 0x550A,
    RECOVER_ASSOCIATED_IMAGE_BYTES  = 0x550B,
    RECOVER_ICC_PROFILE             = 0x550C,
    RECOVER_ANNOTATIONS             = 0x550D,
    RECOVER_ANNOTATION_BYTES        = 0x550F,
};

enum TYPES : uint8_t {
    TYPE_INT8                       = 0,
    TYPE_INT16,
    TYPE_INT32,
    TYPE_INT64,
    TYPE_UINT8,
    TYPE_UINT16,
    TYPE_UINT32,
    TYPE_UINT64,
    TYPE_FLOAT16,
    TYPE_FLOAT32,
    TYPE_FLOAT64,
    TYPE_STRING,
    TYPE_DATE_TIME,
    TYPE_HALF_FLOAT                 = TYPE_FLOAT16,
    TYPE_FLOAT                      = TYPE_FLOAT32,
    TYPE_DOUBLE                     = TYPE_FLOAT64,
};
enum TYPE_SIZES {
    TYPE_SIZE_UINT8                 = 1,
    TYPE_SIZE_UINT16                = 2,
    TYPE_SIZE_UINT24                = 3,
    TYPE_SIZE_UINT32                = 4,
    TYPE_SIZE_UINT40                = 5,
    TYPE_SIZE_UINT64                = 8,
    TYPE_SIZE_FLOAT16               = 2,
    TYPE_SIZE_FLOAT32               = 4,
    TYPE_SIZE_FLOAT64               = 8,
    TYPE_SIZE_INT8                  = TYPE_SIZE_UINT8,
    TYPE_SIZE_INT16                 = TYPE_SIZE_UINT16,
    TYPE_SIZE_INT24                 = TYPE_SIZE_UINT24,
    TYPE_SIZE_INT32                 = TYPE_SIZE_UINT32,
    TUPE_SIZE_INT40                 = TYPE_SIZE_UINT40,
    TYPE_SIZE_INT64                 = TYPE_SIZE_UINT64,
    TYPE_SIZE_DATE_TIME             = TYPE_SIZE_UINT64,
};

// MARK: - HEADER TYPES
// MARK: File Header
/*
 *  BREAKDOWN:
 *  | -------------------------------------------- HEADER ---------------------------------------------|
 *  | MAGIC_BYTES | FILE SIZE | ENCODER VERSION | FILE REVISION NUMBER | TILE TABLE PTR | METADATA PTR | ->>
 *                                                                             |                |---------->
 *                                                                             |--------------------------->
 *  Tile table offset location (ptr) is REQUIRED (ie NOT NULL_OFFSET)
 *  Metadata offset location   (ptr) is REQUIRED even if no metadata is encoded within the table.
 *
 */
struct FILE_HEADER {
    enum vtable_sizes {
        MAGIC_BYTES_OFFSET_S        = TYPE_SIZE_UINT32,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        FILE_SIZE_S                 = TYPE_SIZE_UINT64,
        EXTENSION_MAJOR_S           = TYPE_SIZE_UINT16,
        EXTENSION_MINOR_S           = TYPE_SIZE_UINT16,
        FILE_REVISION_S             = TYPE_SIZE_UINT32,
        TILE_TABLE_OFFSET_S         = TYPE_SIZE_UINT64,
        METADATA_OFFSET_S           = TYPE_SIZE_UINT64,
    };
    enum vtable_offsets {
        MAGIC_BYTES_OFFSET          = 0,
        RECOVERY                    = MAGIC_BYTES_OFFSET + MAGIC_BYTES_OFFSET_S,
        FILE_SIZE                   = RECOVERY + RECOVERY_S,
        EXTENSION_MAJOR             = FILE_SIZE + FILE_SIZE_S,
        EXTENSION_MINOR             = EXTENSION_MAJOR + EXTENSION_MAJOR_S,
        FILE_REVISION               = EXTENSION_MINOR + EXTENSION_MINOR_S,
        TILE_TABLE_OFFSET           = FILE_REVISION + FILE_REVISION_S,
        METADATA_OFFSET             = TILE_TABLE_OFFSET + TILE_TABLE_OFFSET_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = METADATA_OFFSET + METADATA_OFFSET_S,
    };
    operator    bool                () const;
    void        validate_header     (BYTE* const __base) const;
    void        validate_full       (BYTE* const __base) const;
    std::string get_file_info       (BYTE* const __base) const;
    Header      read_header         (BYTE* const __base) const;
    TILE_TABLE  get_tile_table      (BYTE* const __base) const;
    METADATA    get_metadata        (BYTE* const __base) const;
    
    Size        __size              = 0;
    
    explicit FILE_HEADER            (Size file_size) noexcept;
};
struct HeaderCreateInfo {
    size_t      fileSize            = 0;
    uint32_t    revision            = 0;
    Offset      tileTableOffset     = NULL_OFFSET;
    Offset      metadataOffset      = NULL_OFFSET;
};
void STORE_FILE_HEADER              (BYTE* const __base, const HeaderCreateInfo&);

// MARK: Tile Table Header
struct TILE_TABLE {
    friend FILE_HEADER;
    enum vtable_sizes   {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        ENCODING_S                  = TYPE_SIZE_UINT8,
        FORMAT_S                    = TYPE_SIZE_UINT8,
        CIPHER_OFFSET_S             = TYPE_SIZE_UINT64,
        TILE_OFFSETS_OFFSET_S       = TYPE_SIZE_UINT64,
        LAYER_EXTENTS_OFFSET_S      = TYPE_SIZE_UINT64,
        X_EXTENT_S                  = TYPE_SIZE_UINT32,
        Y_EXTENT_S                  = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        ENCODING                    = RECOVERY + RECOVERY_S,
        FORMAT                      = ENCODING + ENCODING_S,
        CIPHER_OFFSET               = FORMAT + FORMAT_S,
        TILE_OFFSETS_OFFSET         = CIPHER_OFFSET + CIPHER_OFFSET_S,
        LAYER_EXTENTS_OFFSET        = TILE_OFFSETS_OFFSET + TILE_OFFSETS_OFFSET_S,
        X_EXTENT                    = LAYER_EXTENTS_OFFSET + LAYER_EXTENTS_OFFSET_S,
        Y_EXTENT                    = X_EXTENT + X_EXTENT_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        TABLE_HEADER_SIZE           = Y_EXTENT + Y_EXTENT_S,
    };
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        validate_full       (BYTE* const __base) const;
    TileTable   read_tile_table     (BYTE* const __base) const;
    LAYER_EXTENTS get_layer_extents (BYTE* const __base) const;
    TILE_OFFSETS  get_tile_offsets  (BYTE* const __base) const;
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
protected:
    explicit    TILE_TABLE          (Offset tile_table_offset, Size file_size, uint32_t version) noexcept;
};
struct TileTableCreateInfo {
    Offset      tileTableOffset     = NULL_OFFSET;
    Encoding    encoding            = TILE_ENCODING_UNDEFINED;
    Format      format              = FORMAT_UNDEFINED;
    Offset      cipherOffset        = NULL_OFFSET;
    Offset      tilesOffset         = NULL_OFFSET;
    Offset      layerExtentsOffset  = NULL_OFFSET;
    uint32_t    layers              = 0;
    uint32_t    widthPixels         = 0;
    uint32_t    heightPixels        = 0;
};
void STORE_TILE_TABLE             (BYTE* const, const TileTableCreateInfo&);

// MARK: Metadata Header
struct METADATA {
    friend FILE_HEADER;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        CODEC_MAJOR_S               = TYPE_SIZE_UINT16,
        CODEC_MINOR_S               = TYPE_SIZE_UINT16,
        CODEC_BUILD_S               = TYPE_SIZE_UINT16,
        ATTRIBUTES_OFFSET_S         = TYPE_SIZE_UINT64,
        IMAGES_OFFSET_S             = TYPE_SIZE_UINT64,
        ICC_COLOR_OFFSET_S          = TYPE_SIZE_UINT64,
        ANNOTATIONS_OFFSET_S        = TYPE_SIZE_UINT64,
        MICRONS_PIXEL_S             = TYPE_SIZE_FLOAT32,
        MAGNIFICATION_S             = TYPE_SIZE_FLOAT32
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        CODEC_MAJOR                 = RECOVERY + RECOVERY_S,
        CODEC_MINOR                 = CODEC_MAJOR + CODEC_MAJOR_S,
        CODEC_BUILD                 = CODEC_MINOR + CODEC_MINOR_S,
        ATTRIBUTES_OFFSET           = CODEC_BUILD + CODEC_BUILD_S,
        IMAGES_OFFSET               = ATTRIBUTES_OFFSET + ATTRIBUTES_OFFSET_S,
        ICC_COLOR_OFFSET            = IMAGES_OFFSET + IMAGES_OFFSET_S,
        ANNOTATIONS_OFFSET          = ICC_COLOR_OFFSET + ICC_COLOR_OFFSET_S,
        MICRONS_PIXEL               = ANNOTATIONS_OFFSET + ANNOTATIONS_OFFSET_S,
        MAGNIFICATION               = MICRONS_PIXEL + MICRONS_PIXEL_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        METADATA_SIZE               = MAGNIFICATION + MAGNIFICATION_S
    };
    
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        validate_full       (BYTE* const __base) const;
    Metadata    read_metadata       (BYTE* const __base) const;
    bool        attributes          (BYTE* const __base) const;
    ATTRIBUTES  get_attributes      (BYTE* const __base) const;
    bool        image_array         (BYTE* const __base) const;
    IMAGE_ARRAY get_image_array     (BYTE* const __base) const;
    bool        color_profile       (BYTE* const __base) const;
    ICC_PROFILE get_color_profile   (BYTE* const __base) const;
    bool                annotations             (BYTE* const __base) const;
    ANNOTATION_ARRAY    get_annotations         (BYTE* const __base) const;
    bool                annotation_groups       (BYTE* const __base) const;
    ANNOTATION_GROUPS   get_annotation_groups   (BYTE* const __base) const;
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
protected:
    explicit    METADATA            () = delete;
    explicit    METADATA            (Offset __metadata, Size file_size, uint32_t version) noexcept;
};
struct MetadataCreateInfo {
    Offset      metadataOffset      = NULL_OFFSET;
    Version     codecVersion        = {0,0,0};
    int16_t     I2Standard          = -1;
    Offset      attributes          = NULL_OFFSET;
    Offset      images              = NULL_OFFSET;
    Offset      ICC_profile         = NULL_OFFSET;
    Offset      annotations         = NULL_OFFSET;
    float       micronsPerPixel     = 0.f;
    float       magnification       = 0.f;
};
void STORE_METADATA                 (BYTE* const __base, const MetadataCreateInfo&);

// MARK: ATTRIBUTES
struct ATTRIBUTES {
    friend METADATA;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        FORMAT_S                    = TYPE_SIZE_UINT8,
        VERSION_S                   = TYPE_SIZE_UINT16,
        NUMBER_S                    = TYPE_SIZE_UINT16,
        LENGTHS_OFFSET_S            = TYPE_SIZE_UINT64,
        BYTE_ARRAY_OFFSET_S         = TYPE_SIZE_UINT64,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        FORMAT                      = RECOVERY + RECOVERY_S,
        VERSION                     = FORMAT + FORMAT_S,
        NUMBER                      = VERSION + VERSION_S,
        LENGTHS_OFFSET              = NUMBER + NUMBER_S,
        BYTE_ARRAY_OFFSET           = LENGTHS_OFFSET + LENGTHS_OFFSET_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        SIZE                        = BYTE_ARRAY_OFFSET + BYTE_ARRAY_OFFSET_S
    };
    
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        validate_full       (BYTE* const __base) const;
    Attributes  read_attributes     (BYTE* const __base) const;
    ATTRIBUTES_SIZES get_sizes      (BYTE* const __base) const;
    ATTRIBUTES_BYTES get_bytes      (BYTE* const __base) const;
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
protected:
    explicit    ATTRIBUTES          () = delete;
    explicit    ATTRIBUTES          (Offset offset, Size file_size, uint32_t version) noexcept;
};
struct AttributesCreateInfo {
    Offset      attributesOffset    = NULL_OFFSET;
    MetadataType format             = METADATA_UNDEFINED;
    uint32_t    version             = 0;
    Offset      sizes               = NULL_OFFSET;
    Offset      bytes               = NULL_OFFSET;
};
void STORE_ATTRIBUTES               (BYTE* const __base, const AttributesCreateInfo&);

// MARK: - ARRAY DATA TYPES
// MARK: LAYER EXTENTS (Slide dimensions)
struct LAYER_EXTENT {
    friend LAYER_EXTENTS;
    enum vtable_sizes {
        X_TILES_S                   = TYPE_SIZE_UINT32,
        Y_TILES_S                   = TYPE_SIZE_UINT32,
        SCALE_S                     = TYPE_SIZE_FLOAT32,
    };
    enum vtable_offsets {
        X_TILES                     = 0,
        Y_TILES                     = X_TILES + X_TILES_S,
        SCALE                       = Y_TILES + Y_TILES_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        SIZE                        = SCALE + SCALE_S
    };
};
struct LAYER_EXTENTS {
    friend TILE_TABLE;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        ENTRY_SIZE_S                = TYPE_SIZE_UINT16,
        ENTRY_NUMBER_S              = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        ENTRY_SIZE                  = RECOVERY + RECOVERY_S,
        ENTRY_NUMBER                = ENTRY_SIZE + ENTRY_SIZE_S,
        HEADER_V1_0_SIZE            = ENTRY_NUMBER + ENTRY_NUMBER_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE
    };
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        validate_full       (BYTE* const __base) const;
    LayerExtents read_layer_extents (BYTE* const __base) const;
    
    Offset      __offset            = NULL_OFFSET;
    uint32_t    __version           = 0;
    
protected:
    explicit LAYER_EXTENTS          (Offset offset, uint32_t version) noexcept;
};
Size SIZE_EXTENTS                   (const LayerExtents&);
void STORE_EXTENTS                  (BYTE* const __base, Offset offset, const LayerExtents&);

// MARK: Tile Offsets (tile lookup table)
struct TILE_OFFSET {
    friend TILE_OFFSETS;
    enum vtable_sizes {
        OFFSET_S                    = TYPE_SIZE_UINT40, // (40-bit FAULTS AT 1TB)
        TILE_SIZE_S                 = TYPE_SIZE_UINT24, // (Tile always < 2^18 bytes)
    };
    enum vtable_offsets {
        OFFSET                      = 0,
        TILE_SIZE                   = OFFSET + OFFSET_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        SIZE                        = TILE_SIZE + TILE_SIZE_S,
    };
};
struct TILE_OFFSETS {
    friend TILE_TABLE;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        ENTRY_SIZE_S                = TYPE_SIZE_INT16,
        ENTRY_NUMBER_S              = TYPE_SIZE_INT32,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        ENTRY_SIZE                  = RECOVERY + RECOVERY_S,
        ENTRY_NUMBER                = ENTRY_SIZE + ENTRY_SIZE_S,
        HEADER_V1_0_SIZE            = ENTRY_NUMBER + ENTRY_NUMBER_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE
    };
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        validate_full       (BYTE* const __base) const;
    void        read_tile_offsets   (BYTE* const __base, TileTable&) const;
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
    
protected:
    explicit TILE_OFFSETS           () = delete;
    explicit TILE_OFFSETS           (Offset offset, Size file_size, uint32_t version) noexcept;
};
Size SIZE_TILE_OFFSETS              (const TileTable::Layers&);
void STORE_TILE_OFFSETS             (BYTE* const, Offset, const TileTable::Layers&);

// MARK: ATTRIBUTES SIZES
struct ATTRIBUTE_SIZE {
    friend ATTRIBUTE_SIZE;
    enum vtable_sizes {
        KEY_SIZE_S                  = TYPE_SIZE_UINT16,
        VALUE_SIZE_S                = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        KEY_SIZE                    = 0,
        VALUE_SIZE                  = KEY_SIZE + KEY_SIZE_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        SIZE                        = VALUE_SIZE + VALUE_SIZE_S,
    };
};
struct ATTRIBUTES_SIZES {
    friend ATTRIBUTES;
    using SizeArray                 = std::vector<std::pair<uint16_t,uint32_t>>;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        ENTRY_SIZE_S                = TYPE_SIZE_UINT16,
        ENTRY_NUMBER_S              = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        ENTRY_SIZE                  = RECOVERY + RECOVERY_S,
        ENTRY_NUMBER                = ENTRY_SIZE + ENTRY_SIZE_S,
        HEADER_V1_0_SIZE            = ENTRY_NUMBER + ENTRY_NUMBER_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE
    };
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        validate_full       (BYTE* const __base) const;
    SizeArray   read_sizes          (BYTE* const __base) const;
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
    
protected:
    explicit ATTRIBUTES_SIZES       () = delete;
    explicit ATTRIBUTES_SIZES       (Offset offset, Size file_size, uint32_t version) noexcept;
};
Size SIZE_ATTRIBUTES_SIZES          (const Attributes&);
void STORE_ATTRIBUTES_SIZES         (BYTE* const __base, Offset, const Attributes&);

// MARK: ATTRIBUTES BYTES

struct ATTRIBUTES_BYTES {
    friend ATTRIBUTES;
    using SizeArray                 = ATTRIBUTES_SIZES::SizeArray;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        ENTRY_NUMBER_S              = TYPE_SIZE_INT32,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        ENTRY_NUMBER                = RECOVERY + RECOVERY_S,
        HEADER_V1_0_SIZE            = ENTRY_NUMBER + ENTRY_NUMBER_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        HEADER_SIZE                 = HEADER_V1_0_SIZE
    };
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        validate_full       (BYTE* const __base) const;
    void        read_bytes          (BYTE* const __base, const SizeArray&, Attributes&) const;
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
    
protected:
    explicit ATTRIBUTES_BYTES       () = delete;
    explicit ATTRIBUTES_BYTES       (Offset offset, Size file_size, uint32_t version) noexcept;
};
Size SIZE_ATTRIBUTES_BYTES          (const Attributes&);
void STORE_ATTRIBUTES_BYTES         (BYTE* const __base, Offset, const Attributes&);

// MARK: - ASSOCIATED IMAGES
// MARK: IMAGES ARRAY
struct IMAGE_ENTRY {
    enum vtable_sizes {
        BYTES_OFFSET_S              = TYPE_SIZE_UINT64,
        WIDTH_S                     = TYPE_SIZE_UINT32,
        HEIGHT_S                    = TYPE_SIZE_UINT32,
        ENCODING_S                  = TYPE_SIZE_UINT8,
        FORMAT_S                    = TYPE_SIZE_UINT8,
        ORIENTATION_S               = TYPE_SIZE_UINT16,
    };
    enum vtable_offsets {
        BYTES_OFFSET                = 0,
        WIDTH                       = BYTES_OFFSET + BYTES_OFFSET_S,
        HEIGHT                      = WIDTH + WIDTH_S,
        ENCODING                    = HEIGHT + HEIGHT_S,
        FORMAT                      = ENCODING + ENCODING_S,
        ORIENTATION                 = FORMAT + FORMAT_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        SIZE                        = ORIENTATION + ORIENTATION_S,
    };
};
struct IMAGE_ARRAY {
    friend METADATA;
    using Labels                    = Metadata::ImageLabels;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        ENTRY_SIZE_S                = TYPE_SIZE_UINT16,
        ENTRY_NUMBER_S              = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        ENTRY_SIZE                  = RECOVERY + RECOVERY_S,
        ENTRY_NUMBER                = ENTRY_SIZE + ENTRY_SIZE_S,
        HEADER_V1_0_SIZE            = ENTRY_NUMBER + ENTRY_NUMBER_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE,
    };
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
    
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        validate_full       (BYTE* const __base) const;
    Images      read_images         (BYTE* const __base) const;
    
protected:
    explicit    IMAGE_ARRAY         () = delete;
    explicit    IMAGE_ARRAY         (Offset offset, Size file_size, uint32_t version) noexcept;
};
struct AssociatedImageCreateInfo {
    struct ImageInfo {
        using       Encoding        = ImageEncoding;
        using       Orientation     = ImageOrientation;
        Offset      offset          = NULL_OFFSET;
        uint32_t    width           = 0;
        uint32_t    height          = 0;
        Encoding    encoding        = IMAGE_ENCODING_UNDEFINED;
        Format      format          = FORMAT_UNDEFINED;
        Orientation orientation     = ORIENTATION_0;
    }; using ImageInfos             = std::vector<ImageInfo>;
    
    Offset      offset              = NULL_OFFSET;
    ImageInfos  images;
};
Size SIZE_IMAGES_ARRAY              (AssociatedImageCreateInfo&);
void STORE_IMAGES_ARRAY             (BYTE* const __base, const AssociatedImageCreateInfo&);

// MARK: IMAGE_BYTES
struct IMAGE_BYTES {
    friend IMAGE_ARRAY;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        TITLE_SIZE_S                = TYPE_SIZE_UINT16,
        IMAGE_SIZE_S                = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        TITLE_SIZE                  = RECOVERY + RECOVERY_S,
        IMAGE_SIZE                  = TITLE_SIZE + TITLE_SIZE_S,
        HEADER_V1_0_SIZE            = IMAGE_SIZE + IMAGE_SIZE_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE,
    };
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
    
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        read_image_bytes    (BYTE* const __base, Abstraction::Image&) const;
    
protected:
    explicit    IMAGE_BYTES         () = delete;
    explicit    IMAGE_BYTES         (Offset offset, Size file_size, uint32_t version) noexcept;
};
struct ImageBytesCreateInfo {
    Offset      offset              = NULL_OFFSET;
    std::string title;
    BYTE* const data                = nullptr;
    size_t      dataBytes           = 0;
};
Size SIZE_IMAGES_BYTES              (const ImageBytesCreateInfo&);
void STORE_IMAGES_BYTES             (BYTE* const __base, const ImageBytesCreateInfo&);

// MARK: - ICC Color Profile

struct ICC_PROFILE {
    friend METADATA;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        ENTRY_NUMBER_S              = TYPE_SIZE_INT32,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        ENTRY_NUMBER                = RECOVERY + RECOVERY_S,
        HEADER_V1_0_SIZE            = ENTRY_NUMBER + ENTRY_NUMBER_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        HEADER_SIZE                 = HEADER_V1_0_SIZE
    };
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    std::string read_profile        (BYTE* const __base) const;
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
    
protected:
    explicit ICC_PROFILE      () = delete;
    explicit ICC_PROFILE      (Offset offset, Size file_size, uint32_t version) noexcept;
};
Size SIZE_ICC_COLOR_PROFILE         (const std::string& color_profile);
void STORE_ICC_COLOR_PROFILE        (BYTE* const __base, Offset, const std::string& color_profile);

// MARK: - Annotations
struct ANNOTATION_ENTRY {
    enum vtable_sizes {
        IDENTIFIER_S                = TYPE_SIZE_UINT24,
        BYTES_OFFSET_S              = TYPE_SIZE_UINT64,
        FORMAT_S                    = TYPE_SIZE_UINT8,
        X_LOCATION_S                = TYPE_SIZE_FLOAT32,
        Y_LOCATION_S                = TYPE_SIZE_FLOAT32,
        X_SIZE_S                    = TYPE_SIZE_FLOAT32,
        Y_SIZE_S                    = TYPE_SIZE_FLOAT32,
        WIDTH_S                     = TYPE_SIZE_UINT32,
        HEIGHT_S                    = TYPE_SIZE_UINT32,
        PARENT_S                    = TYPE_SIZE_UINT24,
    };
    enum vtable_offsets {
        IDENTIFIER                  = 0,
        BYTES_OFFSET                = IDENTIFIER + IDENTIFIER_S,
        FORMAT                      = BYTES_OFFSET + BYTES_OFFSET_S,
        X_LOCATION                  = FORMAT + FORMAT_S,
        Y_LOCATION                  = X_LOCATION + X_LOCATION_S,
        X_SIZE                      = Y_LOCATION + Y_LOCATION_S,
        Y_SIZE                      = X_SIZE + X_SIZE_S,
        WIDTH                       = Y_SIZE + Y_SIZE_S,
        HEIGHT                      = WIDTH + WIDTH_S,
        PARENT                      = HEIGHT + HEIGHT_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        SIZE                        = PARENT + PARENT_S,
    };
};
// MARK: ANNOTATION ARRAY
struct ANNOTATION_ARRAY {
    friend METADATA;
    using Annotations               = Abstraction::Annotations;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        ENTRY_SIZE_S                = TYPE_SIZE_UINT16,
        ENTRY_NUMBER_S              = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        ENTRY_SIZE                  = RECOVERY + RECOVERY_S,
        ENTRY_NUMBER                = ENTRY_SIZE + ENTRY_SIZE_S,
        HEADER_V1_0_SIZE            = ENTRY_NUMBER + ENTRY_NUMBER_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE,
    };
    
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        validate_full       (BYTE* const __base) const;
    Annotations read_annotations    (BYTE* const __base) const;
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
    
protected:
    explicit ANNOTATION_ARRAY       () = delete;
    explicit ANNOTATION_ARRAY       (Offset offset, Size file_size, uint32_t version) noexcept;
};
struct AnnotationArrayCreateInfo {
    using Annotation                = Abstraction::Annotation;
    struct AnnotationInfo {
        using Type                  = AnnotationTypes;
        uint32_t    identifier      = Annotation::NULL_ID;
        Offset      bytesOffset     = NULL_OFFSET;
        Type        type            = ANNOTATION_UNDEFINED;
        float       xLocation       = 0.f;
        float       yLocation       = 0.f;
        float       xSize           = 0.f;
        float       ySize           = 0.f;
        uint32_t    width           = 0;
        uint32_t    height          = 0;
        uint32_t    parent          = Annotation::NULL_ID;
    }; using AnnotationInfos        = std::set<AnnotationInfo>;
    
    Offset          offset          = NULL_OFFSET;
    AnnotationInfos annotations;
};
Size SIZE_ANNOTATION_ARRAY          (const AnnotationArrayCreateInfo&);
void STORE_ANNOTATION_ARRAY         (BYTE* const __base, const AnnotationArrayCreateInfo&);

// MARK: ANNOTATION BYTES
struct ANNOTATION_BYTES {
    friend ANNOTATION_ARRAY;
    using Annotation                = Abstraction::Annotation;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        ENTRY_NUMBER_S              = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        ENTRY_NUMBER                = RECOVERY + RECOVERY_S,
        HEADER_V1_0_SIZE            = ENTRY_NUMBER + ENTRY_NUMBER_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE,
    };
    
    operator    bool                () const;
    void        validate_offset     (BYTE* const __base) const;
    void        read_bytes          (BYTE* const __base, Annotation&) const;
    
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
    
protected:
    explicit ANNOTATION_BYTES       () = delete;
    explicit ANNOTATION_BYTES       (Offset offset, Size file_size, uint32_t version) noexcept;
};
Size SIZE_ANNOTATION_BYTES          (const IrisCodec::Annotation&);
void STORE_ANNOTATION_BYTES         (BYTE* const __base, Offset, const IrisCodec::Annotation&);

// MARK: ANNOTATION GROUPS
struct ANNOTATION_GROUPS {
    
};

} // END FILE STRUCTURE
} // END IRIS CODEC
#endif /* IrisCodecExtension_hpp */
