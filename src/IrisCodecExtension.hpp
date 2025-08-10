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

// Should we export the IFE API for low-level calls to the IFE bytestream.
// If being compiled as a part of another project and you do not want to
// or need to export calls that directly manipulate the byte stream,
// set this preprocessor macro to false.
#ifndef IFE_EXPORT_API
#define IFE_EXPORT_API      false
#endif
#if IFE_EXPORT_API
    #ifndef IFE_EXPORT
    #if defined(_MSC_VER)
    #define IFE_EXPORT      __declspec(dllexport)
    #else
    #define IFE_EXPORT      __attribute__ ((visibility ("default")))
#endif
    #endif
#else
    #ifndef IFE_EXPORT
    #if defined(_MSC_VER)
    #define IFE_EXPORT      __declspec(dllimport)
    #else
    #define IFE_EXPORT      // Default is hidden (see CMakeLists)
    #endif
    #endif
#endif

namespace IrisCodec {
using namespace Iris;
constexpr Offset   NULL_OFFSET          = UINT64_MAX;

// IRIS EXTENSION VERSION FOR WHICH THIS HEADER CORRESPONDS
constexpr uint16_t IRIS_EXTENSION_MAJOR = 1;
constexpr uint16_t IRIS_EXTENSION_MINOR = 0;

// Iris' Magic Number is ASCII for 'Iris' 49 72 69 73
#define MAGIC_BYTES 0x49726973

// These are the header and array datablocks defined in this file:
namespace Serialization {

// MARK: HEADER TYPES
struct FILE_HEADER;
struct TILE_TABLE;
struct METADATA;
struct ATTRIBUTES;
struct ANNOTATIONS;
// Version 1.0 ends here.

// MARK: ARRAY TYPES
struct LAYER_EXTENTS;
struct TILE_OFFSETS;
struct ATTRIBUTES_SIZES;
struct ATTRIBUTES_BYTES;
struct IMAGE_ARRAY;
struct IMAGE_BYTES;
struct ICC_PROFILE;
struct ANNOTATIONS;
struct ANNOTATION_BYTES;
struct ANNOTATION_GROUP_SIZES;
struct ANNOTATION_GROUP_BYTES;
// Version 1.0 ends here.

}
// These are the light-weight RAM representaitons of the on-disk file:
namespace Abstraction {
struct File;
struct FileMap;
}

// MARK: - ENTRY METHODS
#ifndef __EMSCRIPTEN__
/// Perform quick check to see if this file header matches an Iris format. This does NOT validate.
bool IFE_EXPORT is_Iris_Codec_file    (BYTE* const __mapped_file_ptr,
                                       size_t file_size);
/**
 * @brief Performs deep file validation checks to ensure stuctural offsets are valid. This does NOT perform
 * specification validations.
 *
 * This performs a tree validation of objects and sub-objects to ensure their offsets properly.
 */
Result IFE_EXPORT validate_file_structure (BYTE* const __mapped_file_ptr,
                                           size_t file_size) noexcept;
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
Abstraction::File IFE_EXPORT abstract_file_structure (BYTE* const __mapped_file_ptr,
                                                      size_t file_size);
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
Abstraction::FileMap IFE_EXPORT generate_file_map (BYTE* const __mapped_file_ptr,
                                                   size_t file_size);

#elif /* EMSCRIPTEN WEB ASSEMBLY */ defined __EMSCRIPTEN__
using Response = std::shared_ptr<struct __Response>;
/// Perform quick check to see if this file header matches an Iris format. This does NOT validate.
bool IFE_EXPORT is_Iris_Codec_file    (const std::string url,
                                       size_t file_size);
/**
 * @brief Performs deep file validation checks to ensure stuctural offsets are valid. This does NOT perform
 * specification validations.
 *
 * This performs a tree validation of objects and sub-objects to ensure their offsets properly.
 */
Result IFE_EXPORT validate_file_structure (const std::string url,
                                           size_t file_size) noexcept;
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
Abstraction::File IFE_EXPORT abstract_file_structure (const std::string url,
                                                      size_t file_size);
#endif
// MARK: - FILE ABSTRACTIONS
// The file abstractions pull light-weight
// representations of the on-disk information
// such as critial offset locations and sizes
// of larger image or vector payloads
namespace Abstraction {
/**
 * @brief Extracted file header information
 *
 * The extracted version does not contain metadata
 * used to validate the file such as the magic number;
 * this was used internally already to produce the footer
 */
struct IFE_EXPORT Header {
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
struct IFE_EXPORT Cipher {
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
struct IFE_EXPORT TileEntry {
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
struct IFE_EXPORT TileTable {
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
struct IFE_EXPORT AssociatedImage {
    using Info                  = AssociatedImageInfo;
    Offset          offset      = NULL_OFFSET;
    Size            byteSize    = 0;
    Info            info;
};
/**
 * @brief Label-image dictionary for associated images
 */
using AssociatedImages = IFE_EXPORT std::unordered_map<std::string, AssociatedImage>;
/**
 * @brief Annotation abstraction containing on-slide annotations by annotation
 * identifier (24-bit value) and annotation groups by group name (string)
 *
 */
struct IFE_EXPORT Annotation {
    using       Identifier  = Iris::Annotation::Identifier;
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
struct IFE_EXPORT AnnotationGroup {
    Offset      offset      = NULL_OFFSET;
    uint32_t    number      = 0;
    Size        byteSize    () {return number * 3;}
};
struct IFE_EXPORT Annotations :
public std::unordered_map<Annotation::Identifier, Annotation> {
    using       Groups = std::unordered_map<std::string, AnnotationGroup>;
    Groups      groups;
};
/**
 * @brief In-memory abstraction of the Iris file structure
 *
 * This is a low-overhead file abstraction that allows for
 * fast access to the underlying slide data.
 */
struct IFE_EXPORT File {
    Header              header;
    TileTable           tileTable;
    AssociatedImages    images;
    Annotations         annotations;
    Metadata            metadata;
};
struct IFE_EXPORT FileMap :
public std::map<Offset, struct FileMapEntry> {
    Size                file_size   = 0;
};
}
// MARK: - IRIS CODEC EXTENSION SERIALIZATION TYPES
namespace Serialization {
using namespace Abstraction;
using MagicBytes                    = uint_least32_t;

/**
 * @brief Iris Codec statically definied offset values
 * IFE Specification Section 2.2.0
 */
enum IFE_EXPORT Offsets : uint_least64_t {
    HEADER_OFFSET                   = 0,
    NULL_OFFSET                     = 18446744073709551615ULL,
    NULL_TILE                       = 1099511627775ULL,
};
/**
 * @brief Iris Codec Files contain methods to
 * heal corrupted metadata in the event of errors
 *
 * IFE Specification Section 2.2.1
 *
 */
enum IFE_EXPORT RECOVERY : uint_least16_t {
    // In the event of recovery, we will search
    // for a byte offset that stores its own value
    // followed by one of these sequences.
    RECOVER_UNDEFINED               = 0x5500,
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
    RECOVER_ANNOTATION_BYTES        = 0x550E,
    RECOVER_ANNOTATION_GROUP_SIZES  = 0x550F,
    RECOVER_ANNOTATION_GROUP_BYTES  = 0x5510,
};
enum IFE_EXPORT TYPE_SIZES {
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
struct IFE_EXPORT DATA_BLOCK {
    enum vtable_sizes   {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        HEADER_SIZE                 = RECOVERY + RECOVERY_S
    };
#ifdef /* WEB ASSEMBLY */ __EMSCRIPTEN__
    Offset      __remote            = NULL_OFFSET;
    Response    __response          = NULL;
#endif
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
    operator    bool                () const;
    explicit    DATA_BLOCK          (){};
    explicit    DATA_BLOCK          (Offset, Size file_size, uint32_t IFE_version);
    Result      validate_offset     (const BYTE* const __base, const char*,
                                     enum RECOVERY) const noexcept;
    Size        validate_bounds     () const noexcept;
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
struct IFE_EXPORT FILE_HEADER : DATA_BLOCK {
    static constexpr
    char type []                    = "FILE_HEADER";
    static const
    enum RECOVERY    recovery       = RECOVER_HEADER;
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
        HEADER_V1_0_SIZE            = METADATA_OFFSET + METADATA_OFFSET_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE
    };
    Size        size                (const BYTE* const __base) const;
    Result      validate_header     (const BYTE* const __base) const;
    Result      validate_full       (const BYTE* const __base) const noexcept;
    Header      read_header         (const BYTE* const __base) const;
    TILE_TABLE  get_tile_table      (const BYTE* const __base) const;
    METADATA    get_metadata        (const BYTE* const __base) const;
    
    explicit FILE_HEADER            (Size file_size) noexcept;
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
#ifndef __EMSCRIPTEN__
struct IFE_EXPORT HeaderCreateInfo {
    size_t      fileSize            = 0;
    uint32_t    revision            = 0;
    Offset      tileTableOffset     = NULL_OFFSET;
    Offset      metadataOffset      = NULL_OFFSET;
};
void STORE_FILE_HEADER              (BYTE* const __base, const HeaderCreateInfo&);
#endif
// MARK: Tile Table Header
struct IFE_EXPORT TILE_TABLE : DATA_BLOCK {
    friend FILE_HEADER;
    static constexpr
    char type []                    = "TILE_TABLE";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_TILE_TABLE;
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
        HEADER_V1_0_SIZE            = Y_EXTENT + Y_EXTENT_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE,
    };
    Size        size                () const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base) const noexcept;
    TileTable   read_tile_table     (const BYTE* const __base) const;
    LAYER_EXTENTS get_layer_extents (const BYTE* const __base) const;
    TILE_OFFSETS  get_tile_offsets  (const BYTE* const __base) const;
    
protected:
    explicit    TILE_TABLE          (Offset tile_table_offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
struct IFE_EXPORT TileTableCreateInfo {
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
void STORE_TILE_TABLE               (BYTE* const __base, const TileTableCreateInfo&);

// MARK: Metadata Header
struct IFE_EXPORT METADATA : DATA_BLOCK {
    friend FILE_HEADER;
    static constexpr
    char type []                    = "METADATA";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_METADATA;
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
        HEADER_V1_0_SIZE            = MAGNIFICATION + MAGNIFICATION_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE,
    };
    
    Size        size                () const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base) const noexcept;
    Size        get_size            (const BYTE* const __base) const;
    Metadata    read_metadata       (const BYTE* const __base) const;
    bool        attributes          (const BYTE* const __base) const;
    ATTRIBUTES  get_attributes      (const BYTE* const __base) const;
    bool        image_array         (const BYTE* const __base) const;
    IMAGE_ARRAY get_image_array     (const BYTE* const __base) const;
    bool        color_profile       (const BYTE* const __base) const;
    ICC_PROFILE get_color_profile   (const BYTE* const __base) const;
    bool        annotations         (const BYTE* const __base) const;
    ANNOTATIONS get_annotations     (const BYTE* const __base) const;
    
protected:
    explicit    METADATA            () = delete;
    explicit    METADATA            (Offset __metadata, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
struct IFE_EXPORT MetadataCreateInfo {
    Offset      metadataOffset      = NULL_OFFSET;
    Version     codecVersion        = {0,0,0};
    Offset      attributes          = NULL_OFFSET;
    Offset      images              = NULL_OFFSET;
    Offset      ICC_profile         = NULL_OFFSET;
    Offset      annotations         = NULL_OFFSET;
    float       micronsPerPixel     = 0.f;
    float       magnification       = 0.f;
};
void IFE_EXPORT STORE_METADATA      (BYTE* const __base, const MetadataCreateInfo&);

// MARK: ATTRIBUTES
struct IFE_EXPORT ATTRIBUTES : DATA_BLOCK {
    friend METADATA;
    static constexpr
    char type []                    = "ATTRIBUTES";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_ATTRIBUTES;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        FORMAT_S                    = TYPE_SIZE_UINT8,
        VERSION_S                   = TYPE_SIZE_UINT16,
        LENGTHS_OFFSET_S            = TYPE_SIZE_UINT64,
        BYTE_ARRAY_OFFSET_S         = TYPE_SIZE_UINT64,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        FORMAT                      = RECOVERY + RECOVERY_S,
        VERSION                     = FORMAT + FORMAT_S,
        LENGTHS_OFFSET              = VERSION + VERSION_S,
        BYTE_ARRAY_OFFSET           = LENGTHS_OFFSET + LENGTHS_OFFSET_S,
        HEADER_V1_0_SIZE            = BYTE_ARRAY_OFFSET + BYTE_ARRAY_OFFSET_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE,
    };
    Size        size                () const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base) const noexcept;
    Attributes  read_attributes     (const BYTE* const __base) const;
    ATTRIBUTES_SIZES get_sizes      (const BYTE* const __base) const;
    ATTRIBUTES_BYTES get_bytes      (const BYTE* const __base) const;

protected:
    explicit    ATTRIBUTES          () = delete;
    explicit    ATTRIBUTES          (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
struct IFE_EXPORT AttributesCreateInfo {
    Offset      attributesOffset    = NULL_OFFSET;
    MetadataType type               = METADATA_UNDEFINED;
    uint32_t    version             = 0;
    Offset      sizes               = NULL_OFFSET;
    Offset      bytes               = NULL_OFFSET;
};
void STORE_ATTRIBUTES               (BYTE* const __base, const AttributesCreateInfo&);

// MARK: - ARRAY DATA TYPES
// MARK: LAYER EXTENTS (Slide dimensions)
struct IFE_EXPORT LAYER_EXTENT {
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
struct IFE_EXPORT LAYER_EXTENTS : DATA_BLOCK {
    friend TILE_TABLE;
    static constexpr
    char type []                    = "LAYER_EXTENTS";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_LAYER_EXTENTS;
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
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base) const noexcept;
    LayerExtents read_layer_extents (const BYTE* const __base) const;
    
protected:
    explicit LAYER_EXTENTS          (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
Size IFE_EXPORT SIZE_EXTENTS        (const LayerExtents&);
void IFE_EXPORT STORE_EXTENTS       (BYTE* const __base, Offset offset, const LayerExtents&);

// MARK: Tile Offsets (tile lookup table)
struct IFE_EXPORT TILE_OFFSET {
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
struct IFE_EXPORT TILE_OFFSETS : DATA_BLOCK {
    friend TILE_TABLE;
    static constexpr
    char type []                    = "TILE_OFFSETS";
    static constexpr
    enum RECOVERY    recovery       = RECOVER_TILE_OFFSETS;
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
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base) const noexcept;
    void        read_tile_offsets   (const BYTE* const __base, TileTable&) const;
    
protected:
    explicit TILE_OFFSETS           () = delete;
    explicit TILE_OFFSETS           (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
Size IFE_EXPORT SIZE_TILE_OFFSETS   (const TileTable::Layers&);
void IFE_EXPORT STORE_TILE_OFFSETS  (BYTE* const, Offset, const TileTable::Layers&);

// MARK: ATTRIBUTES SIZES
struct IFE_EXPORT ATTRIBUTE_SIZE {
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
struct IFE_EXPORT ATTRIBUTES_SIZES : DATA_BLOCK {
    friend ATTRIBUTES;
    static constexpr
    char type []                    = "ATTRIBUTES_SIZES";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_ATTRIBUTES_SIZES;
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
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base, Size& expected_bytes) const noexcept;
    SizeArray   read_sizes          (const BYTE* const __base) const;
    
protected:
    explicit ATTRIBUTES_SIZES       () = delete;
    explicit ATTRIBUTES_SIZES       (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
Size IFE_EXPORT SIZE_ATTRIBUTES_SIZES   (const Attributes&);
void IFE_EXPORT STORE_ATTRIBUTES_SIZES  (BYTE* const __base, Offset, const Attributes&);

// MARK: ATTRIBUTES BYTES

struct IFE_EXPORT ATTRIBUTES_BYTES : DATA_BLOCK {
    friend ATTRIBUTES;
    using SizeArray                 = ATTRIBUTES_SIZES::SizeArray;
    static constexpr
    char type []                    = "ATTRIBUTES_BYTES";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_ATTRIBUTES_BYTES;
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
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base, Size expected_bytes) const noexcept;
    void        read_bytes          (const BYTE* const __base, const SizeArray&, Attributes&) const;
    
protected:
    explicit ATTRIBUTES_BYTES       () = delete;
    explicit ATTRIBUTES_BYTES       (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
Size IFE_EXPORT SIZE_ATTRIBUTES_BYTES   (const Attributes&);
void IFE_EXPORT STORE_ATTRIBUTES_BYTES  (BYTE* const __base, Offset, const Attributes&);

// MARK: - ASSOCIATED IMAGES
// MARK: IMAGES ARRAY
struct IFE_EXPORT IMAGE_ENTRY {
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
struct IFE_EXPORT IMAGE_ARRAY : DATA_BLOCK {
    friend METADATA;
    using Images                    = Abstraction::AssociatedImages;
    using Labels                    = Metadata::ImageLabels;
    using BYTES_ARRAY               = std::vector<IMAGE_BYTES>;
    static constexpr
    char type []                    = "IMAGE_ARRAY";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_ASSOCIATED_IMAGES;
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
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base) const noexcept;
    Images      read_assoc_images   (const BYTE* const __base, BYTES_ARRAY* = nullptr) const;
    
protected:
    explicit    IMAGE_ARRAY         () = delete;
    explicit    IMAGE_ARRAY         (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
struct IFE_EXPORT AssociatedImageCreateInfo {
    struct IFE_EXPORT Entry {
        Offset              offset  = NULL_OFFSET;
        AssociatedImageInfo info;
    }; using Entries                = std::vector<Entry>;
    
    Offset      offset              = NULL_OFFSET;
    Entries     images;
};
Size SIZE_IMAGES_ARRAY              (AssociatedImageCreateInfo&);
void STORE_IMAGES_ARRAY             (BYTE* const __base, const AssociatedImageCreateInfo&);

// MARK: IMAGE_BYTES
struct IFE_EXPORT IMAGE_BYTES : DATA_BLOCK {
    friend IMAGE_ARRAY;
    static constexpr
    char type []                    = "IMAGE_BYTES";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_ASSOCIATED_IMAGE_BYTES;
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
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base) const noexcept;
    std::string read_image_bytes    (const BYTE* const __base, Abstraction::AssociatedImage&) const;
    
protected:
    explicit    IMAGE_BYTES         () = delete;
    explicit    IMAGE_BYTES         (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
struct IFE_EXPORT ImageBytesCreateInfo {
    Offset      offset              = NULL_OFFSET;
    std::string title;
    BYTE* const data                = nullptr;
    size_t      dataBytes           = 0;
};
Size IFE_EXPORT SIZE_IMAGES_BYTES   (const ImageBytesCreateInfo&);
void IFE_EXPORT STORE_IMAGES_BYTES  (BYTE* const __base, const ImageBytesCreateInfo&);

// MARK: - ICC Color Profile

struct IFE_EXPORT ICC_PROFILE : DATA_BLOCK {
    friend METADATA;
    static constexpr
    char type []                    = "ICC_PROFILE";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_ICC_PROFILE;
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
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base) const noexcept;
    std::string read_profile        (const BYTE* const __base) const;
    
protected:
    explicit ICC_PROFILE      () = delete;
    explicit ICC_PROFILE      (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
Size SIZE_ICC_COLOR_PROFILE         (const std::string& color_profile);
void STORE_ICC_COLOR_PROFILE        (BYTE* const __base, Offset, const std::string& color_profile);

// MARK: - Annotation Arrays
struct IFE_EXPORT ANNOTATION_ENTRY {
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
struct IFE_EXPORT ANNOTATIONS : DATA_BLOCK {
    friend METADATA;
    using Annotations               = Abstraction::Annotations;
    using BYTES_ARRAY               = std::vector<ANNOTATION_BYTES>;
    using GROUP_SIZES               = ANNOTATION_GROUP_SIZES;
    using GROUP_BYTES               = ANNOTATION_GROUP_BYTES;
    static constexpr
    char type []                    = "ANNOTATIONS";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_ANNOTATIONS;
    enum vtable_sizes {
        VALIDATION_S                = TYPE_SIZE_UINT64,
        RECOVERY_S                  = TYPE_SIZE_UINT16,
        ENTRY_SIZE_S                = TYPE_SIZE_UINT16,
        ENTRY_NUMBER_S              = TYPE_SIZE_UINT32,
        GROUP_SIZES_OFFSET_S        = TYPE_SIZE_UINT64,
        GROUP_BYTES_OFFSET_S        = TYPE_SIZE_UINT64,
    };
    enum vtable_offsets {
        VALIDATION                  = 0,
        RECOVERY                    = VALIDATION + VALIDATION_S,
        ENTRY_SIZE                  = RECOVERY + RECOVERY_S,
        ENTRY_NUMBER                = ENTRY_SIZE + ENTRY_SIZE_S,
        GROUP_SIZES_OFFSET          = ENTRY_NUMBER + ENTRY_NUMBER_S,
        GROUP_BYTES_OFFSET          = GROUP_SIZES_OFFSET + GROUP_SIZES_OFFSET_S,
        HEADER_V1_0_SIZE            = GROUP_BYTES_OFFSET + GROUP_BYTES_OFFSET_S,
        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        HEADER_SIZE                 = HEADER_V1_0_SIZE,
    };
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base) const noexcept;
    Annotations read_annotations    (const BYTE* const __base, BYTES_ARRAY* = nullptr) const;
    
    
    bool        groups              (const BYTE* const __base) const;
    GROUP_SIZES get_group_sizes     (const BYTE* const __base) const;
    GROUP_BYTES get_group_bytes     (const BYTE* const __base) const;

    
protected:
    explicit ANNOTATIONS            () = delete;
    explicit ANNOTATIONS            (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
struct IFE_EXPORT AnnotationArrayCreateInfo {
    using Annotation                = Abstraction::Annotation;
    struct IFE_EXPORT AnnotationInfo {
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
Size IFE_EXPORT SIZE_ANNOTATION_ARRAY   (const AnnotationArrayCreateInfo&);
#ifndef __EMSCRIPTEN__
void IFE_EXPORT STORE_ANNOTATION_ARRAY  (BYTE* const __base, const AnnotationArrayCreateInfo&);
#endif

// MARK: ANNOTATION BYTES
struct IFE_EXPORT ANNOTATION_BYTES : DATA_BLOCK {
    friend ANNOTATIONS;
    using Annotation                = Abstraction::Annotation;
    static constexpr
    char type []                    = "ANNOTATION_BYTES";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_ANNOTATION_BYTES;
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
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    void        read_bytes          (const BYTE* const __base, Annotation&) const;
    
protected:
    explicit ANNOTATION_BYTES       () = delete;
    explicit ANNOTATION_BYTES       (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
Size IFE_EXPORT SIZE_ANNOTATION_BYTES   (const IrisCodec::Annotation&);
#ifndef __EMSCRIPTEN__
void IFE_EXPORT STORE_ANNOTATION_BYTES  (BYTE* const __base, Offset, const IrisCodec::Annotation&);
#endif

// MARK: ANNOTATION GROUPS
struct IFE_EXPORT ANNOTATION_GROUP_SIZE {
    enum vtable_sizes {
        LABEL_SIZE_S                = TYPE_SIZE_UINT16,
        ENTRIES_NUMBER_S            = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        LABEL_SIZE                  = 0,
        ENTRIES_NUMBER              = LABEL_SIZE + LABEL_SIZE_S,

        // Version 1.0 ends here.
        // -----------------------------------------------------------------------
        
        SIZE                        = ENTRIES_NUMBER + ENTRIES_NUMBER_S,
    };
};
struct IFE_EXPORT ANNOTATION_GROUP_SIZES : DATA_BLOCK {
    friend ANNOTATIONS;
    using GroupSizes                = std::vector<std::pair<uint16_t,uint32_t>>;
    using Groups                    = Abstraction::Annotations::Groups;
    static constexpr
    char type []                    = "ANNOTATION_GROUP_SIZES";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_ANNOTATION_GROUP_SIZES;
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
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base, Size& expected_bytes) const noexcept;
    GroupSizes  read_group_sizes    (const BYTE* const __base) const;
    
protected:
    explicit ANNOTATION_GROUP_SIZES () = delete;
    explicit ANNOTATION_GROUP_SIZES (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
struct IFE_EXPORT ANNOTATION_GROUP_BYTES : DATA_BLOCK {
    friend ANNOTATIONS;
    using GroupSizes                = ANNOTATION_GROUP_SIZES::GroupSizes;
    using Annotations               = Abstraction::Annotations;
    static constexpr
    char type []                    = "ANNOTATION_GROUP_BYTES";
    static constexpr enum
    RECOVERY    recovery            = RECOVER_ANNOTATION_GROUP_BYTES;
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
    Size        size                (const BYTE* const __base) const;
    Result      validate_offset     (const BYTE* const __base) const noexcept;
    Result      validate_full       (const BYTE* const __base, Size total_size) const noexcept;
    void        read_bytes          (const BYTE* const __base, const GroupSizes&, Annotations&) const;
    
protected:
    explicit ANNOTATION_GROUP_BYTES () = delete;
    explicit ANNOTATION_GROUP_BYTES (Offset offset, Size file_size, uint32_t version) noexcept;
private:
    #ifdef __EMSCRIPTEN__
    void check_and_fetch_remote     (const BYTE* const& __base);
    #endif
};
} // END FILE STRUCTURE


namespace Abstraction {
enum IFE_EXPORT MapEntryType {
    MAP_ENTRY_UNDEFINED         = 0,
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
};
/**
 * @brief FileMap entry representing a datablock within the IFE file structure system.
 */
struct IFE_EXPORT FileMapEntry {
    using Datablock                 = Serialization::DATA_BLOCK;
    MapEntryType        type        = MAP_ENTRY_UNDEFINED;
    Datablock           datablock;
    Size                size        = 0;
};
}
} // END IRIS CODEC
#endif /* IrisCodecExtension_hpp */
