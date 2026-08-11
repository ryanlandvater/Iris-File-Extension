/**
 * @file ife_wire_parity_tests.cpp
 * @brief Compile-time proof that the generated layout equals shipped IFE 1.0.
 *
 * Every assertion below compares a generated offset or size against the
 * hand-written vtable it must reproduce. There is no runtime component: if
 * this translation unit compiles, the two layouts agree, and if a schema edit
 * ever moves a byte the build fails naming the field.
 *
 * This file is deliberately temporary. It exists to gate the migration and
 * dies with src/IrisCodecExtension.*, having served as the only
 * mechanical proof that the generated layer speaks the shipped format.
 *
 * What is asserted is the **1.0 prefix**: the sizes and offsets of fields that
 * shipped in IFE 1.0. A later version appending fields is legal and must not
 * fail this wall, so it compares header_size_v1_0 rather than header_size —
 * the newest-version total legitimately grows.
 *
 * Field names differ in places (ENTRY_SIZE became STRIDE, ENTRY_NUMBER became
 * COUNT, ATTRIBUTES gained clearer offset names); the pairing below is the
 * complete record of those renames. Bytes are unaffected by any of them.
 */
// Include order matters, and not by accident: IrisCodecExtension.hpp says
// `using namespace Iris;` but does not include the headers that define those
// types - src/IrisCodecExtension.cpp:42-45 includes them first and the header
// relies on that having happened. Reproduce the same order here. The generated
// headers deliberately do not inherit this fragility: they are self-contained.
#include <cstdint>
#include <type_traits>

#include "IrisTypes.hpp"
#include "IrisCodecTypes.hpp"

#include "IrisCodecExtension.hpp"

#include "IFE_Bytes.hpp"
#include "IFE_VTables.hpp"
#include "IFE_Constants.hpp"

namespace {
namespace v1  = IrisCodec::Serialization;
namespace gen = IFE::vtables;
}  // namespace

// Both layers' magic numbers are visible here at once, which is the whole
// point of MAGIC_BYTES no longer being a macro: a macro has no namespace and
// clobbered the generated constant, so a consumer could not migrate one file
// at a time. This assertion fails to compile if the macro ever returns.
static_assert(IFE::constants::MAGIC_BYTES == IrisCodec::MAGIC_BYTES,
              "MAGIC_BYTES: the generated and hand-written values diverged");


// ---- FILE_HEADER vs v1 FILE_HEADER ----
static_assert(gen::FILE_HEADER::header_size_v1_0 == v1::FILE_HEADER::HEADER_V1_0_SIZE,
              "FILE_HEADER: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::FILE_HEADER::offset::MAGIC == v1::FILE_HEADER::MAGIC_BYTES_OFFSET,
              "FILE_HEADER.MAGIC: offset diverged from shipped IFE 1.0");
static_assert(gen::FILE_HEADER::offset::RECOVERY == v1::FILE_HEADER::RECOVERY,
              "FILE_HEADER.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::FILE_HEADER::offset::FILE_SIZE == v1::FILE_HEADER::FILE_SIZE,
              "FILE_HEADER.FILE_SIZE: offset diverged from shipped IFE 1.0");
static_assert(gen::FILE_HEADER::offset::EXTENSION_MAJOR == v1::FILE_HEADER::EXTENSION_MAJOR,
              "FILE_HEADER.EXTENSION_MAJOR: offset diverged from shipped IFE 1.0");
static_assert(gen::FILE_HEADER::offset::EXTENSION_MINOR == v1::FILE_HEADER::EXTENSION_MINOR,
              "FILE_HEADER.EXTENSION_MINOR: offset diverged from shipped IFE 1.0");
static_assert(gen::FILE_HEADER::offset::FILE_REVISION == v1::FILE_HEADER::FILE_REVISION,
              "FILE_HEADER.FILE_REVISION: offset diverged from shipped IFE 1.0");
static_assert(gen::FILE_HEADER::offset::TILE_TABLE_OFFSET == v1::FILE_HEADER::TILE_TABLE_OFFSET,
              "FILE_HEADER.TILE_TABLE_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::FILE_HEADER::offset::METADATA_OFFSET == v1::FILE_HEADER::METADATA_OFFSET,
              "FILE_HEADER.METADATA_OFFSET: offset diverged from shipped IFE 1.0");

// ---- TILE_TABLE vs v1 TILE_TABLE ----
static_assert(gen::TILE_TABLE::header_size_v1_0 == v1::TILE_TABLE::HEADER_V1_0_SIZE,
              "TILE_TABLE: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::BLOCK::offset::VALIDATION == v1::TILE_TABLE::VALIDATION,
              "TILE_TABLE.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::BLOCK::offset::RECOVERY == v1::TILE_TABLE::RECOVERY,
              "TILE_TABLE.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_TABLE::offset::ENCODING == v1::TILE_TABLE::ENCODING,
              "TILE_TABLE.ENCODING: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_TABLE::offset::FORMAT == v1::TILE_TABLE::FORMAT,
              "TILE_TABLE.FORMAT: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_TABLE::offset::CIPHER_OFFSET == v1::TILE_TABLE::CIPHER_OFFSET,
              "TILE_TABLE.CIPHER_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_TABLE::offset::TILE_OFFSETS_OFFSET == v1::TILE_TABLE::TILE_OFFSETS_OFFSET,
              "TILE_TABLE.TILE_OFFSETS_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_TABLE::offset::LAYER_EXTENTS_OFFSET == v1::TILE_TABLE::LAYER_EXTENTS_OFFSET,
              "TILE_TABLE.LAYER_EXTENTS_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_TABLE::offset::X_EXTENT == v1::TILE_TABLE::X_EXTENT,
              "TILE_TABLE.X_EXTENT: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_TABLE::offset::Y_EXTENT == v1::TILE_TABLE::Y_EXTENT,
              "TILE_TABLE.Y_EXTENT: offset diverged from shipped IFE 1.0");

// ---- METADATA vs v1 METADATA ----
static_assert(gen::METADATA::header_size_v1_0 == v1::METADATA::HEADER_V1_0_SIZE,
              "METADATA: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::BLOCK::offset::VALIDATION == v1::METADATA::VALIDATION,
              "METADATA.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::BLOCK::offset::RECOVERY == v1::METADATA::RECOVERY,
              "METADATA.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::METADATA::offset::CODEC_MAJOR == v1::METADATA::CODEC_MAJOR,
              "METADATA.CODEC_MAJOR: offset diverged from shipped IFE 1.0");
static_assert(gen::METADATA::offset::CODEC_MINOR == v1::METADATA::CODEC_MINOR,
              "METADATA.CODEC_MINOR: offset diverged from shipped IFE 1.0");
static_assert(gen::METADATA::offset::CODEC_BUILD == v1::METADATA::CODEC_BUILD,
              "METADATA.CODEC_BUILD: offset diverged from shipped IFE 1.0");
static_assert(gen::METADATA::offset::ATTRIBUTES_OFFSET == v1::METADATA::ATTRIBUTES_OFFSET,
              "METADATA.ATTRIBUTES_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::METADATA::offset::IMAGES_OFFSET == v1::METADATA::IMAGES_OFFSET,
              "METADATA.IMAGES_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::METADATA::offset::ICC_COLOR_OFFSET == v1::METADATA::ICC_COLOR_OFFSET,
              "METADATA.ICC_COLOR_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::METADATA::offset::ANNOTATIONS_OFFSET == v1::METADATA::ANNOTATIONS_OFFSET,
              "METADATA.ANNOTATIONS_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::METADATA::offset::MICRONS_PIXEL == v1::METADATA::MICRONS_PIXEL,
              "METADATA.MICRONS_PIXEL: offset diverged from shipped IFE 1.0");
static_assert(gen::METADATA::offset::MAGNIFICATION == v1::METADATA::MAGNIFICATION,
              "METADATA.MAGNIFICATION: offset diverged from shipped IFE 1.0");

// ---- ATTRIBUTES vs v1 ATTRIBUTES ----
static_assert(gen::ATTRIBUTES::header_size_v1_0 == v1::ATTRIBUTES::HEADER_V1_0_SIZE,
              "ATTRIBUTES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::BLOCK::offset::VALIDATION == v1::ATTRIBUTES::VALIDATION,
              "ATTRIBUTES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::BLOCK::offset::RECOVERY == v1::ATTRIBUTES::RECOVERY,
              "ATTRIBUTES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTES::offset::FORMAT == v1::ATTRIBUTES::FORMAT,
              "ATTRIBUTES.FORMAT: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTES::offset::VERSION == v1::ATTRIBUTES::VERSION,
              "ATTRIBUTES.VERSION: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTES::offset::SIZES_OFFSET == v1::ATTRIBUTES::LENGTHS_OFFSET,
              "ATTRIBUTES.SIZES_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTES::offset::BYTES_OFFSET == v1::ATTRIBUTES::BYTE_ARRAY_OFFSET,
              "ATTRIBUTES.BYTES_OFFSET: offset diverged from shipped IFE 1.0");

// ---- LAYER_EXTENTS vs v1 LAYER_EXTENTS ----
static_assert(gen::LAYER_EXTENTS::header_size_v1_0 == v1::LAYER_EXTENTS::HEADER_V1_0_SIZE,
              "LAYER_EXTENTS: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::VALIDATION == v1::LAYER_EXTENTS::VALIDATION,
              "LAYER_EXTENTS.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::RECOVERY == v1::LAYER_EXTENTS::RECOVERY,
              "LAYER_EXTENTS.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::STRIDE == v1::LAYER_EXTENTS::ENTRY_SIZE,
              "LAYER_EXTENTS.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::COUNT == v1::LAYER_EXTENTS::ENTRY_NUMBER,
              "LAYER_EXTENTS.COUNT: offset diverged from shipped IFE 1.0");
static_assert(gen::LAYER_EXTENTS::entry_size_v1_0 == v1::LAYER_EXTENT::SIZE,
              "LAYER_EXTENT: entry size diverged from shipped IFE 1.0");
static_assert(gen::LAYER_EXTENTS::entry::offset::X_TILES == v1::LAYER_EXTENT::X_TILES,
              "LAYER_EXTENT.X_TILES: offset diverged from shipped IFE 1.0");
static_assert(gen::LAYER_EXTENTS::entry::offset::Y_TILES == v1::LAYER_EXTENT::Y_TILES,
              "LAYER_EXTENT.Y_TILES: offset diverged from shipped IFE 1.0");
static_assert(gen::LAYER_EXTENTS::entry::offset::SCALE == v1::LAYER_EXTENT::SCALE,
              "LAYER_EXTENT.SCALE: offset diverged from shipped IFE 1.0");

// ---- TILE_OFFSETS vs v1 TILE_OFFSETS ----
static_assert(gen::TILE_OFFSETS::header_size_v1_0 == v1::TILE_OFFSETS::HEADER_V1_0_SIZE,
              "TILE_OFFSETS: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::VALIDATION == v1::TILE_OFFSETS::VALIDATION,
              "TILE_OFFSETS.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::RECOVERY == v1::TILE_OFFSETS::RECOVERY,
              "TILE_OFFSETS.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::STRIDE == v1::TILE_OFFSETS::ENTRY_SIZE,
              "TILE_OFFSETS.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::COUNT == v1::TILE_OFFSETS::ENTRY_NUMBER,
              "TILE_OFFSETS.COUNT: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_OFFSETS::entry_size_v1_0 == v1::TILE_OFFSET::SIZE,
              "TILE_OFFSET: entry size diverged from shipped IFE 1.0");
static_assert(gen::TILE_OFFSETS::entry::offset::OFFSET == v1::TILE_OFFSET::OFFSET,
              "TILE_OFFSET.OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_OFFSETS::entry::offset::SIZE == v1::TILE_OFFSET::TILE_SIZE,
              "TILE_OFFSET.SIZE: offset diverged from shipped IFE 1.0");

// ---- ATTRIBUTE_SIZES vs v1 ATTRIBUTES_SIZES ----
static_assert(gen::ATTRIBUTE_SIZES::header_size_v1_0 == v1::ATTRIBUTES_SIZES::HEADER_V1_0_SIZE,
              "ATTRIBUTE_SIZES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::VALIDATION == v1::ATTRIBUTES_SIZES::VALIDATION,
              "ATTRIBUTE_SIZES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::RECOVERY == v1::ATTRIBUTES_SIZES::RECOVERY,
              "ATTRIBUTE_SIZES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::STRIDE == v1::ATTRIBUTES_SIZES::ENTRY_SIZE,
              "ATTRIBUTE_SIZES.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::COUNT == v1::ATTRIBUTES_SIZES::ENTRY_NUMBER,
              "ATTRIBUTE_SIZES.COUNT: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTE_SIZES::entry_size_v1_0 == v1::ATTRIBUTE_SIZE::SIZE,
              "ATTRIBUTE_SIZE: entry size diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTE_SIZES::entry::offset::KEY_SIZE == v1::ATTRIBUTE_SIZE::KEY_SIZE,
              "ATTRIBUTE_SIZE.KEY_SIZE: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTE_SIZES::entry::offset::VALUE_SIZE == v1::ATTRIBUTE_SIZE::VALUE_SIZE,
              "ATTRIBUTE_SIZE.VALUE_SIZE: offset diverged from shipped IFE 1.0");

// ---- ATTRIBUTE_BYTES vs v1 ATTRIBUTES_BYTES ----
static_assert(gen::ATTRIBUTE_BYTES::header_size_v1_0 == v1::ATTRIBUTES_BYTES::HEADER_V1_0_SIZE,
              "ATTRIBUTE_BYTES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::VALIDATION == v1::ATTRIBUTES_BYTES::VALIDATION,
              "ATTRIBUTE_BYTES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::RECOVERY == v1::ATTRIBUTES_BYTES::RECOVERY,
              "ATTRIBUTE_BYTES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::COUNT == v1::ATTRIBUTES_BYTES::ENTRY_NUMBER,
              "ATTRIBUTE_BYTES.COUNT: offset diverged from shipped IFE 1.0");

// ---- IMAGES vs v1 IMAGE_ARRAY ----
static_assert(gen::IMAGES::header_size_v1_0 == v1::IMAGE_ARRAY::HEADER_V1_0_SIZE,
              "IMAGES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::VALIDATION == v1::IMAGE_ARRAY::VALIDATION,
              "IMAGES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::RECOVERY == v1::IMAGE_ARRAY::RECOVERY,
              "IMAGES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::STRIDE == v1::IMAGE_ARRAY::ENTRY_SIZE,
              "IMAGES.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::COUNT == v1::IMAGE_ARRAY::ENTRY_NUMBER,
              "IMAGES.COUNT: offset diverged from shipped IFE 1.0");

// ---- IMAGE_BYTES vs v1 IMAGE_BYTES ----
static_assert(gen::IMAGE_BYTES::header_size_v1_0 == v1::IMAGE_BYTES::HEADER_V1_0_SIZE,
              "IMAGE_BYTES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::BLOCK::offset::VALIDATION == v1::IMAGE_BYTES::VALIDATION,
              "IMAGE_BYTES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::BLOCK::offset::RECOVERY == v1::IMAGE_BYTES::RECOVERY,
              "IMAGE_BYTES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::IMAGE_BYTES::offset::TITLE_SIZE == v1::IMAGE_BYTES::TITLE_SIZE,
              "IMAGE_BYTES.TITLE_SIZE: offset diverged from shipped IFE 1.0");
static_assert(gen::IMAGE_BYTES::offset::IMAGE_SIZE == v1::IMAGE_BYTES::IMAGE_SIZE,
              "IMAGE_BYTES.IMAGE_SIZE: offset diverged from shipped IFE 1.0");

// ---- ICC_PROFILE vs v1 ICC_PROFILE ----
static_assert(gen::ICC_PROFILE::header_size_v1_0 == v1::ICC_PROFILE::HEADER_V1_0_SIZE,
              "ICC_PROFILE: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::VALIDATION == v1::ICC_PROFILE::VALIDATION,
              "ICC_PROFILE.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::RECOVERY == v1::ICC_PROFILE::RECOVERY,
              "ICC_PROFILE.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::COUNT == v1::ICC_PROFILE::ENTRY_NUMBER,
              "ICC_PROFILE.COUNT: offset diverged from shipped IFE 1.0");

// ---- ANNOTATIONS vs v1 ANNOTATIONS ----
static_assert(gen::ANNOTATIONS::header_size_v1_0 == v1::ANNOTATIONS::HEADER_V1_0_SIZE,
              "ANNOTATIONS: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::VALIDATION == v1::ANNOTATIONS::VALIDATION,
              "ANNOTATIONS.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::RECOVERY == v1::ANNOTATIONS::RECOVERY,
              "ANNOTATIONS.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::STRIDE == v1::ANNOTATIONS::ENTRY_SIZE,
              "ANNOTATIONS.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::COUNT == v1::ANNOTATIONS::ENTRY_NUMBER,
              "ANNOTATIONS.COUNT: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATIONS::offset::GROUP_SIZES_OFFSET == v1::ANNOTATIONS::GROUP_SIZES_OFFSET,
              "ANNOTATIONS.GROUP_SIZES_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATIONS::offset::GROUP_BYTES_OFFSET == v1::ANNOTATIONS::GROUP_BYTES_OFFSET,
              "ANNOTATIONS.GROUP_BYTES_OFFSET: offset diverged from shipped IFE 1.0");

// ---- ANNOTATION_BYTES vs v1 ANNOTATION_BYTES ----
static_assert(gen::ANNOTATION_BYTES::header_size_v1_0 == v1::ANNOTATION_BYTES::HEADER_V1_0_SIZE,
              "ANNOTATION_BYTES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::VALIDATION == v1::ANNOTATION_BYTES::VALIDATION,
              "ANNOTATION_BYTES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::RECOVERY == v1::ANNOTATION_BYTES::RECOVERY,
              "ANNOTATION_BYTES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::COUNT == v1::ANNOTATION_BYTES::ENTRY_NUMBER,
              "ANNOTATION_BYTES.COUNT: offset diverged from shipped IFE 1.0");

// ---- ANNOTATION_GROUP_SIZES vs v1 ANNOTATION_GROUP_SIZES ----
static_assert(gen::ANNOTATION_GROUP_SIZES::header_size_v1_0 == v1::ANNOTATION_GROUP_SIZES::HEADER_V1_0_SIZE,
              "ANNOTATION_GROUP_SIZES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::VALIDATION == v1::ANNOTATION_GROUP_SIZES::VALIDATION,
              "ANNOTATION_GROUP_SIZES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::RECOVERY == v1::ANNOTATION_GROUP_SIZES::RECOVERY,
              "ANNOTATION_GROUP_SIZES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::STRIDE == v1::ANNOTATION_GROUP_SIZES::ENTRY_SIZE,
              "ANNOTATION_GROUP_SIZES.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::ARRAY::offset::COUNT == v1::ANNOTATION_GROUP_SIZES::ENTRY_NUMBER,
              "ANNOTATION_GROUP_SIZES.COUNT: offset diverged from shipped IFE 1.0");

// ---- ANNOTATION_GROUP_BYTES vs v1 ANNOTATION_GROUP_BYTES ----
static_assert(gen::ANNOTATION_GROUP_BYTES::header_size_v1_0 == v1::ANNOTATION_GROUP_BYTES::HEADER_V1_0_SIZE,
              "ANNOTATION_GROUP_BYTES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::VALIDATION == v1::ANNOTATION_GROUP_BYTES::VALIDATION,
              "ANNOTATION_GROUP_BYTES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::RECOVERY == v1::ANNOTATION_GROUP_BYTES::RECOVERY,
              "ANNOTATION_GROUP_BYTES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::BYTE_ARRAY::offset::COUNT == v1::ANNOTATION_GROUP_BYTES::ENTRY_NUMBER,
              "ANNOTATION_GROUP_BYTES.COUNT: offset diverged from shipped IFE 1.0");

// Recovery tags are unchanged in value; only their names were clarified.
static_assert(static_cast<std::uint16_t>(gen::FILE_HEADER::recovery_tag) == v1::RECOVER_HEADER,
              "FILE_HEADER recovery tag diverged from shipped IFE 1.0");
static_assert(static_cast<std::uint16_t>(gen::TILE_TABLE::recovery_tag) == v1::RECOVER_TILE_TABLE,
              "TILE_TABLE recovery tag diverged from shipped IFE 1.0");


// ============================================================================
// Enumeration values vs shipped IFE 1.0
// ============================================================================
//
// An enumerator is as much a wire value as an offset is: it is the byte a
// field holds. Until this section existed the wall proved where a field sits
// and said nothing about what may legally go in it, and that gap is not
// hypothetical — the schema was authored without ANNOTATION_JPEG, silently
// shifting SVG to 2 and TEXT to 3, and every offset assertion passed. It was
// caught by reading, not by building. Below, it would not compile.
//
// Scope is the same 1.0 prefix the layout assertions use: every enumerator
// IFE 1.0 shipped, compared against the header that shipped it. Members added
// in 1.1 have no v1 counterpart and appear only in the exclusion list.
//
// v1 spells several of these differently and defines them in three places:
// Iris:: (IrisTypes.hpp), IrisCodec:: (IrisCodecTypes.hpp) and
// IrisCodec::Serialization:: (IrisCodecExtension.hpp). The pairing below is
// the complete record of those renames; no value is affected by any of them.

namespace {
namespace con = IFE::constants;

/// The generated enums are scoped; v1's are not. Comparing them requires one
/// cast, and doing it here keeps 38 assertions readable.
template <typename E>
constexpr auto raw(E e) noexcept {
    return static_cast<std::underlying_type_t<E>>(e);
}
}  // namespace

// ---- tile_encodings vs v1 IrisCodec::Encoding ----
// v1's TILE_ENCODING_DEFAULT is an alias of JPEG, not a distinct wire value,
// and the schema forbids aliases outright (--validate rejects two members
// sharing a value). It is deliberately absent here and there.
static_assert(raw(con::TileEncodings::TILE_ENCODING_UNDEFINED) == IrisCodec::TILE_ENCODING_UNDEFINED,
              "TILE_ENCODING_UNDEFINED: value diverged from shipped IFE 1.0");
static_assert(raw(con::TileEncodings::TILE_ENCODING_IRIS) == IrisCodec::TILE_ENCODING_IRIS,
              "TILE_ENCODING_IRIS: value diverged from shipped IFE 1.0");
static_assert(raw(con::TileEncodings::TILE_ENCODING_JPEG) == IrisCodec::TILE_ENCODING_JPEG,
              "TILE_ENCODING_JPEG: value diverged from shipped IFE 1.0");
static_assert(raw(con::TileEncodings::TILE_ENCODING_AVIF) == IrisCodec::TILE_ENCODING_AVIF,
              "TILE_ENCODING_AVIF: value diverged from shipped IFE 1.0");

// ---- pixel_formats vs v1 Iris::Format ----
static_assert(raw(con::PixelFormats::FORMAT_UNDEFINED) == Iris::FORMAT_UNDEFINED,
              "FORMAT_UNDEFINED: value diverged from shipped IFE 1.0");
static_assert(raw(con::PixelFormats::FORMAT_B8G8R8) == Iris::FORMAT_B8G8R8,
              "FORMAT_B8G8R8: value diverged from shipped IFE 1.0");
static_assert(raw(con::PixelFormats::FORMAT_R8G8B8) == Iris::FORMAT_R8G8B8,
              "FORMAT_R8G8B8: value diverged from shipped IFE 1.0");
static_assert(raw(con::PixelFormats::FORMAT_B8G8R8A8) == Iris::FORMAT_B8G8R8A8,
              "FORMAT_B8G8R8A8: value diverged from shipped IFE 1.0");
static_assert(raw(con::PixelFormats::FORMAT_R8G8B8A8) == Iris::FORMAT_R8G8B8A8,
              "FORMAT_R8G8B8A8: value diverged from shipped IFE 1.0");

// ---- annotation_types vs v1 Iris::AnnotationTypes ----
// The four assertions that would have caught the missing ANNOTATION_JPEG.
static_assert(raw(con::AnnotationTypes::ANNOTATION_UNDEFINED) == Iris::ANNOTATION_UNDEFINED,
              "ANNOTATION_UNDEFINED: value diverged from shipped IFE 1.0");
static_assert(raw(con::AnnotationTypes::ANNOTATION_PNG) == Iris::ANNOTATION_PNG,
              "ANNOTATION_PNG: value diverged from shipped IFE 1.0");
static_assert(raw(con::AnnotationTypes::ANNOTATION_JPEG) == Iris::ANNOTATION_JPEG,
              "ANNOTATION_JPEG: value diverged from shipped IFE 1.0");
static_assert(raw(con::AnnotationTypes::ANNOTATION_SVG) == Iris::ANNOTATION_SVG,
              "ANNOTATION_SVG: value diverged from shipped IFE 1.0");
static_assert(raw(con::AnnotationTypes::ANNOTATION_TEXT) == Iris::ANNOTATION_TEXT,
              "ANNOTATION_TEXT: value diverged from shipped IFE 1.0");

// ---- image_encodings vs v1 IrisCodec::ImageEncoding ----
// IMAGE_ENCODING_DEFAULT is an alias, excluded for the reason given above.
static_assert(raw(con::ImageEncodings::IMAGE_ENCODING_UNDEFINED) == IrisCodec::IMAGE_ENCODING_UNDEFINED,
              "IMAGE_ENCODING_UNDEFINED: value diverged from shipped IFE 1.0");
static_assert(raw(con::ImageEncodings::IMAGE_ENCODING_PNG) == IrisCodec::IMAGE_ENCODING_PNG,
              "IMAGE_ENCODING_PNG: value diverged from shipped IFE 1.0");
static_assert(raw(con::ImageEncodings::IMAGE_ENCODING_JPEG) == IrisCodec::IMAGE_ENCODING_JPEG,
              "IMAGE_ENCODING_JPEG: value diverged from shipped IFE 1.0");
static_assert(raw(con::ImageEncodings::IMAGE_ENCODING_AVIF) == IrisCodec::IMAGE_ENCODING_AVIF,
              "IMAGE_ENCODING_AVIF: value diverged from shipped IFE 1.0");

// ---- metadata_formats vs v1 IrisCodec::MetadataType ----
// METADATA_FREE_TEXT is **excluded, deliberately**. v1 defines it as an alias
// of METADATA_I2S (both 1); the schema assigns it 3, recorded as an errata on
// the member in spec/ife_constants.json. Asserting it would fail, and
// "correcting" the schema to 1 would reintroduce an alias --validate rejects.
// Leave it out. What it means for a v1 file that meant free text — it reads as
// an I2S conformance claim now — is an open question recorded under Phase 6 in
// MIGRATION.md, not something to reconcile here.
static_assert(raw(con::MetadataFormats::METADATA_UNDEFINED) == IrisCodec::METADATA_UNDEFINED,
              "METADATA_UNDEFINED: value diverged from shipped IFE 1.0");
static_assert(raw(con::MetadataFormats::METADATA_I2S) == IrisCodec::METADATA_I2S,
              "METADATA_I2S: value diverged from shipped IFE 1.0");
static_assert(raw(con::MetadataFormats::METADATA_DICOM) == IrisCodec::METADATA_DICOM,
              "METADATA_DICOM: value diverged from shipped IFE 1.0");

// ---- recovery_codes vs v1 IrisCodec::Serialization::RECOVERY ----
// Five renames, no value changes: HEADER -> FILE_HEADER, ATTRIBUTES_SIZES ->
// ATTRIBUTE_SIZES, ATTRIBUTES_BYTES -> ATTRIBUTE_BYTES, ASSOCIATED_IMAGES ->
// IMAGES, ASSOCIATED_IMAGE_BYTES -> IMAGE_BYTES. The two per-block
// recovery_tag assertions above cover FILE_HEADER and TILE_TABLE from the
// block side; these cover the enumeration itself, all seventeen of it.
static_assert(raw(con::RecoveryCodes::RECOVER_UNDEFINED) == v1::RECOVER_UNDEFINED,
              "RECOVER_UNDEFINED: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_FILE_HEADER) == v1::RECOVER_HEADER,
              "RECOVER_FILE_HEADER: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_TILE_TABLE) == v1::RECOVER_TILE_TABLE,
              "RECOVER_TILE_TABLE: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_CIPHER) == v1::RECOVER_CIPHER,
              "RECOVER_CIPHER: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_METADATA) == v1::RECOVER_METADATA,
              "RECOVER_METADATA: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_ATTRIBUTES) == v1::RECOVER_ATTRIBUTES,
              "RECOVER_ATTRIBUTES: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_LAYER_EXTENTS) == v1::RECOVER_LAYER_EXTENTS,
              "RECOVER_LAYER_EXTENTS: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_TILE_OFFSETS) == v1::RECOVER_TILE_OFFSETS,
              "RECOVER_TILE_OFFSETS: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_ATTRIBUTE_SIZES) == v1::RECOVER_ATTRIBUTES_SIZES,
              "RECOVER_ATTRIBUTE_SIZES: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_ATTRIBUTE_BYTES) == v1::RECOVER_ATTRIBUTES_BYTES,
              "RECOVER_ATTRIBUTE_BYTES: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_IMAGES) == v1::RECOVER_ASSOCIATED_IMAGES,
              "RECOVER_IMAGES: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_IMAGE_BYTES) == v1::RECOVER_ASSOCIATED_IMAGE_BYTES,
              "RECOVER_IMAGE_BYTES: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_ICC_PROFILE) == v1::RECOVER_ICC_PROFILE,
              "RECOVER_ICC_PROFILE: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_ANNOTATIONS) == v1::RECOVER_ANNOTATIONS,
              "RECOVER_ANNOTATIONS: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_ANNOTATION_BYTES) == v1::RECOVER_ANNOTATION_BYTES,
              "RECOVER_ANNOTATION_BYTES: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_ANNOTATION_GROUP_SIZES) == v1::RECOVER_ANNOTATION_GROUP_SIZES,
              "RECOVER_ANNOTATION_GROUP_SIZES: value diverged from shipped IFE 1.0");
static_assert(raw(con::RecoveryCodes::RECOVER_ANNOTATION_GROUP_BYTES) == v1::RECOVER_ANNOTATION_GROUP_BYTES,
              "RECOVER_ANNOTATION_GROUP_BYTES: value diverged from shipped IFE 1.0");

// ---- image_orientations vs v1 IrisCodec::ImageOrientation ----
// The only enumeration whose two layers disagree in representation rather than
// value: v1 publishes binary16 bit patterns, the schema emits the degrees they
// decode to. Rounding the float back is what makes them comparable, and it is
// a compile-time comparison only because IFE::float_to_half is constexpr.
// Asserting this direction (float -> pattern) keeps the comparison integral;
// comparing decoded floats would compare floats.
static_assert(IFE::float_to_half(con::ORIENTATION_0) == IrisCodec::ORIENTATION_0,
              "ORIENTATION_0: binary16 pattern diverged from shipped IFE 1.0");
static_assert(IFE::float_to_half(con::ORIENTATION_90) == IrisCodec::ORIENTATION_90,
              "ORIENTATION_90: binary16 pattern diverged from shipped IFE 1.0");
static_assert(IFE::float_to_half(con::ORIENTATION_180) == IrisCodec::ORIENTATION_180,
              "ORIENTATION_180: binary16 pattern diverged from shipped IFE 1.0");
static_assert(IFE::float_to_half(con::ORIENTATION_270) == IrisCodec::ORIENTATION_270,
              "ORIENTATION_270: binary16 pattern diverged from shipped IFE 1.0");

// The three negative orientations are **excluded, and not because they are
// wrong**. v1 aliases each to its positive equivalent — ORIENTATION_minus_90
// *is* ORIENTATION_270, one pattern — while the schema encodes the literal
// negative: -90 is 0xD5A0, not 0x5C38. Both decode to the same rotation, since
// the field is degrees interpreted modulo 360, so no file is misread either
// way; they are simply not the same sixteen bits and an assertion here would
// be asserting that two spellings of one rotation are one spelling.
static_assert(IFE::float_to_half(con::ORIENTATION_MINUS_90) != IrisCodec::ORIENTATION_minus_90,
              "ORIENTATION_MINUS_90: v1 aliased this to +270; if the patterns now "
              "agree the schema changed and the exclusion above is stale");

// Not asserted, for want of anything to assert against: CLINICAL_UNDEFINED,
// CLINICAL_HL7_V2, CLINICAL_FHIR_JSON, CLINICAL_FASTFHIR and
// RECOVER_CLINICAL_METADATA are 1.1 additions with no v1 counterpart. When v1
// is retired this whole file goes with it; until then, that is the boundary.

int main() { return 0; }   // compile-time assertions only; nothing to run

