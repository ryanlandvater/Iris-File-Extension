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
 * dies with src/IrisCodecExtension.* in Phase 6, having served as the only
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

#include "IrisTypes.hpp"
#include "IrisCodecTypes.hpp"

#include "IrisCodecExtension.hpp"

// src/IrisCodecExtension.hpp:85 defines MAGIC_BYTES as a preprocessor macro,
// which clobbers the generated `constexpr std::uint32_t MAGIC_BYTES`. The two
// layers therefore cannot currently coexist in one translation unit without
// this #undef. Harmless here - the assertions below use neither - but it is a
// real obstacle for any consumer migrating gradually, and the macro should be
// retired with the rest of the hand-written layer in Phase 6.
#undef MAGIC_BYTES

#include "IFE_VTables.hpp"

namespace {
namespace v1  = IrisCodec::Serialization;
namespace gen = IFE::vtables;
}  // namespace


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
static_assert(gen::TILE_TABLE::offset::VALIDATION == v1::TILE_TABLE::VALIDATION,
              "TILE_TABLE.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_TABLE::offset::RECOVERY == v1::TILE_TABLE::RECOVERY,
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
static_assert(gen::METADATA::offset::VALIDATION == v1::METADATA::VALIDATION,
              "METADATA.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::METADATA::offset::RECOVERY == v1::METADATA::RECOVERY,
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
static_assert(gen::ATTRIBUTES::offset::VALIDATION == v1::ATTRIBUTES::VALIDATION,
              "ATTRIBUTES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTES::offset::RECOVERY == v1::ATTRIBUTES::RECOVERY,
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
static_assert(gen::LAYER_EXTENTS::offset::VALIDATION == v1::LAYER_EXTENTS::VALIDATION,
              "LAYER_EXTENTS.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::LAYER_EXTENTS::offset::RECOVERY == v1::LAYER_EXTENTS::RECOVERY,
              "LAYER_EXTENTS.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::LAYER_EXTENTS::offset::STRIDE == v1::LAYER_EXTENTS::ENTRY_SIZE,
              "LAYER_EXTENTS.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::LAYER_EXTENTS::offset::COUNT == v1::LAYER_EXTENTS::ENTRY_NUMBER,
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
static_assert(gen::TILE_OFFSETS::offset::VALIDATION == v1::TILE_OFFSETS::VALIDATION,
              "TILE_OFFSETS.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_OFFSETS::offset::RECOVERY == v1::TILE_OFFSETS::RECOVERY,
              "TILE_OFFSETS.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_OFFSETS::offset::STRIDE == v1::TILE_OFFSETS::ENTRY_SIZE,
              "TILE_OFFSETS.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::TILE_OFFSETS::offset::COUNT == v1::TILE_OFFSETS::ENTRY_NUMBER,
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
static_assert(gen::ATTRIBUTE_SIZES::offset::VALIDATION == v1::ATTRIBUTES_SIZES::VALIDATION,
              "ATTRIBUTE_SIZES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTE_SIZES::offset::RECOVERY == v1::ATTRIBUTES_SIZES::RECOVERY,
              "ATTRIBUTE_SIZES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTE_SIZES::offset::STRIDE == v1::ATTRIBUTES_SIZES::ENTRY_SIZE,
              "ATTRIBUTE_SIZES.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTE_SIZES::offset::COUNT == v1::ATTRIBUTES_SIZES::ENTRY_NUMBER,
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
static_assert(gen::ATTRIBUTE_BYTES::offset::VALIDATION == v1::ATTRIBUTES_BYTES::VALIDATION,
              "ATTRIBUTE_BYTES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTE_BYTES::offset::RECOVERY == v1::ATTRIBUTES_BYTES::RECOVERY,
              "ATTRIBUTE_BYTES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ATTRIBUTE_BYTES::offset::COUNT == v1::ATTRIBUTES_BYTES::ENTRY_NUMBER,
              "ATTRIBUTE_BYTES.COUNT: offset diverged from shipped IFE 1.0");

// ---- IMAGES vs v1 IMAGE_ARRAY ----
static_assert(gen::IMAGES::header_size_v1_0 == v1::IMAGE_ARRAY::HEADER_V1_0_SIZE,
              "IMAGES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::IMAGES::offset::VALIDATION == v1::IMAGE_ARRAY::VALIDATION,
              "IMAGES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::IMAGES::offset::RECOVERY == v1::IMAGE_ARRAY::RECOVERY,
              "IMAGES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::IMAGES::offset::STRIDE == v1::IMAGE_ARRAY::ENTRY_SIZE,
              "IMAGES.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::IMAGES::offset::COUNT == v1::IMAGE_ARRAY::ENTRY_NUMBER,
              "IMAGES.COUNT: offset diverged from shipped IFE 1.0");

// ---- IMAGE_BYTES vs v1 IMAGE_BYTES ----
static_assert(gen::IMAGE_BYTES::header_size_v1_0 == v1::IMAGE_BYTES::HEADER_V1_0_SIZE,
              "IMAGE_BYTES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::IMAGE_BYTES::offset::VALIDATION == v1::IMAGE_BYTES::VALIDATION,
              "IMAGE_BYTES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::IMAGE_BYTES::offset::RECOVERY == v1::IMAGE_BYTES::RECOVERY,
              "IMAGE_BYTES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::IMAGE_BYTES::offset::TITLE_SIZE == v1::IMAGE_BYTES::TITLE_SIZE,
              "IMAGE_BYTES.TITLE_SIZE: offset diverged from shipped IFE 1.0");
static_assert(gen::IMAGE_BYTES::offset::IMAGE_SIZE == v1::IMAGE_BYTES::IMAGE_SIZE,
              "IMAGE_BYTES.IMAGE_SIZE: offset diverged from shipped IFE 1.0");

// ---- ICC_PROFILE vs v1 ICC_PROFILE ----
static_assert(gen::ICC_PROFILE::header_size_v1_0 == v1::ICC_PROFILE::HEADER_V1_0_SIZE,
              "ICC_PROFILE: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ICC_PROFILE::offset::VALIDATION == v1::ICC_PROFILE::VALIDATION,
              "ICC_PROFILE.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ICC_PROFILE::offset::RECOVERY == v1::ICC_PROFILE::RECOVERY,
              "ICC_PROFILE.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ICC_PROFILE::offset::COUNT == v1::ICC_PROFILE::ENTRY_NUMBER,
              "ICC_PROFILE.COUNT: offset diverged from shipped IFE 1.0");

// ---- ANNOTATIONS vs v1 ANNOTATIONS ----
static_assert(gen::ANNOTATIONS::header_size_v1_0 == v1::ANNOTATIONS::HEADER_V1_0_SIZE,
              "ANNOTATIONS: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATIONS::offset::VALIDATION == v1::ANNOTATIONS::VALIDATION,
              "ANNOTATIONS.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATIONS::offset::RECOVERY == v1::ANNOTATIONS::RECOVERY,
              "ANNOTATIONS.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATIONS::offset::STRIDE == v1::ANNOTATIONS::ENTRY_SIZE,
              "ANNOTATIONS.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATIONS::offset::COUNT == v1::ANNOTATIONS::ENTRY_NUMBER,
              "ANNOTATIONS.COUNT: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATIONS::offset::GROUP_SIZES_OFFSET == v1::ANNOTATIONS::GROUP_SIZES_OFFSET,
              "ANNOTATIONS.GROUP_SIZES_OFFSET: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATIONS::offset::GROUP_BYTES_OFFSET == v1::ANNOTATIONS::GROUP_BYTES_OFFSET,
              "ANNOTATIONS.GROUP_BYTES_OFFSET: offset diverged from shipped IFE 1.0");

// ---- ANNOTATION_BYTES vs v1 ANNOTATION_BYTES ----
static_assert(gen::ANNOTATION_BYTES::header_size_v1_0 == v1::ANNOTATION_BYTES::HEADER_V1_0_SIZE,
              "ANNOTATION_BYTES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATION_BYTES::offset::VALIDATION == v1::ANNOTATION_BYTES::VALIDATION,
              "ANNOTATION_BYTES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATION_BYTES::offset::RECOVERY == v1::ANNOTATION_BYTES::RECOVERY,
              "ANNOTATION_BYTES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATION_BYTES::offset::COUNT == v1::ANNOTATION_BYTES::ENTRY_NUMBER,
              "ANNOTATION_BYTES.COUNT: offset diverged from shipped IFE 1.0");

// ---- ANNOTATION_GROUP_SIZES vs v1 ANNOTATION_GROUP_SIZES ----
static_assert(gen::ANNOTATION_GROUP_SIZES::header_size_v1_0 == v1::ANNOTATION_GROUP_SIZES::HEADER_V1_0_SIZE,
              "ANNOTATION_GROUP_SIZES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATION_GROUP_SIZES::offset::VALIDATION == v1::ANNOTATION_GROUP_SIZES::VALIDATION,
              "ANNOTATION_GROUP_SIZES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATION_GROUP_SIZES::offset::RECOVERY == v1::ANNOTATION_GROUP_SIZES::RECOVERY,
              "ANNOTATION_GROUP_SIZES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATION_GROUP_SIZES::offset::STRIDE == v1::ANNOTATION_GROUP_SIZES::ENTRY_SIZE,
              "ANNOTATION_GROUP_SIZES.STRIDE: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATION_GROUP_SIZES::offset::COUNT == v1::ANNOTATION_GROUP_SIZES::ENTRY_NUMBER,
              "ANNOTATION_GROUP_SIZES.COUNT: offset diverged from shipped IFE 1.0");

// ---- ANNOTATION_GROUP_BYTES vs v1 ANNOTATION_GROUP_BYTES ----
static_assert(gen::ANNOTATION_GROUP_BYTES::header_size_v1_0 == v1::ANNOTATION_GROUP_BYTES::HEADER_V1_0_SIZE,
              "ANNOTATION_GROUP_BYTES: the 1.0 header size diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATION_GROUP_BYTES::offset::VALIDATION == v1::ANNOTATION_GROUP_BYTES::VALIDATION,
              "ANNOTATION_GROUP_BYTES.VALIDATION: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATION_GROUP_BYTES::offset::RECOVERY == v1::ANNOTATION_GROUP_BYTES::RECOVERY,
              "ANNOTATION_GROUP_BYTES.RECOVERY: offset diverged from shipped IFE 1.0");
static_assert(gen::ANNOTATION_GROUP_BYTES::offset::COUNT == v1::ANNOTATION_GROUP_BYTES::ENTRY_NUMBER,
              "ANNOTATION_GROUP_BYTES.COUNT: offset diverged from shipped IFE 1.0");

// Recovery tags are unchanged in value; only their names were clarified.
static_assert(static_cast<std::uint16_t>(gen::FILE_HEADER::recovery_tag) == v1::RECOVER_HEADER,
              "FILE_HEADER recovery tag diverged from shipped IFE 1.0");
static_assert(static_cast<std::uint16_t>(gen::TILE_TABLE::recovery_tag) == v1::RECOVER_TILE_TABLE,
              "TILE_TABLE recovery tag diverged from shipped IFE 1.0");

int main() { return 0; }   // 101 compile-time assertions; nothing to run

