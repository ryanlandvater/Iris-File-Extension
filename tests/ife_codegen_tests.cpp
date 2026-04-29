/**
 * @file ife_codegen_tests.cpp
 * @brief Phase 3 parity test for the schema-driven codegen.
 *
 * Cross-checks the generated `IFE::vtables::*` namespaces (produced by
 * `tools/ifc.py` from `schema/ife_v1.json`) against the hand-written
 * `enum vtable_offsets` / `enum vtable_sizes` blocks inside
 * `IrisCodec::Serialization`. Every offset, every size, every header_size,
 * and every recovery_tag must match. If anything drifts (e.g. a schema
 * edit adds/removes a field, or someone hand-edits an enum), the build
 * fails at `static_assert` time with a precise message.
 *
 * Most checks are compile-time. A trivial `main` is included so the test
 * also runs under ctest as a sanity probe.
 */
#include "IrisFileExtension.hpp"
#include "IFE_Types.hpp"
#include "IFE_VTables.hpp"
#include "IFE_DataTypes.hpp"
#include "IFE_FieldKeys.hpp"

#include <cstdio>

namespace L  = ::IrisCodec::Serialization;
namespace G  = ::IFE::vtables;
namespace GD = ::IFE::datatypes;
namespace GK = ::IFE::field_keys;

// -----------------------------------------------------------------------------
// Per-resource parity helpers — one `static_assert` per legacy field constant
// against the generated equivalent. The macros expand inline so failures point
// at the exact field that drifted.
// -----------------------------------------------------------------------------
#define IFE_CHECK_OFFSET(RES, FIELD) \
    static_assert(L::RES::FIELD == G::RES::offset::FIELD, \
                  "vtable offset drift: " #RES "::" #FIELD)

#define IFE_CHECK_SIZE(RES, FIELD) \
    static_assert(L::RES::FIELD##_S == G::RES::size::FIELD, \
                  "vtable size drift: " #RES "::" #FIELD "_S")

#define IFE_CHECK_HEADER_SIZE(RES) \
    static_assert(L::RES::HEADER_SIZE == G::RES::header_size, \
                  "header_size drift: " #RES "::HEADER_SIZE")

// Resource recovery tag: the generated `recovery_tag` (a partitioned
// IFE::RECOVERY_TAG) must round-trip via tag_to_legacy back to the legacy
// resource's constexpr `recovery` member (an IrisCodec::Serialization::RECOVERY).
#define IFE_CHECK_RECOVERY(RES) \
    static_assert(::IFE::tag_to_legacy(G::RES::recovery_tag) == L::RES::recovery, \
                  "recovery tag drift: " #RES)

// ---- FILE_HEADER ----
IFE_CHECK_OFFSET(FILE_HEADER, MAGIC_BYTES_OFFSET);
IFE_CHECK_OFFSET(FILE_HEADER, RECOVERY);
IFE_CHECK_OFFSET(FILE_HEADER, FILE_SIZE);
IFE_CHECK_OFFSET(FILE_HEADER, EXTENSION_MAJOR);
IFE_CHECK_OFFSET(FILE_HEADER, EXTENSION_MINOR);
IFE_CHECK_OFFSET(FILE_HEADER, FILE_REVISION);
IFE_CHECK_OFFSET(FILE_HEADER, TILE_TABLE_OFFSET);
IFE_CHECK_OFFSET(FILE_HEADER, METADATA_OFFSET);
IFE_CHECK_SIZE  (FILE_HEADER, MAGIC_BYTES_OFFSET);
IFE_CHECK_SIZE  (FILE_HEADER, RECOVERY);
IFE_CHECK_SIZE  (FILE_HEADER, FILE_SIZE);
IFE_CHECK_SIZE  (FILE_HEADER, EXTENSION_MAJOR);
IFE_CHECK_SIZE  (FILE_HEADER, EXTENSION_MINOR);
IFE_CHECK_SIZE  (FILE_HEADER, FILE_REVISION);
IFE_CHECK_SIZE  (FILE_HEADER, TILE_TABLE_OFFSET);
IFE_CHECK_SIZE  (FILE_HEADER, METADATA_OFFSET);
IFE_CHECK_HEADER_SIZE(FILE_HEADER);
IFE_CHECK_RECOVERY(FILE_HEADER);

// ---- TILE_TABLE ----
IFE_CHECK_OFFSET(TILE_TABLE, VALIDATION);
IFE_CHECK_OFFSET(TILE_TABLE, RECOVERY);
IFE_CHECK_OFFSET(TILE_TABLE, ENCODING);
IFE_CHECK_OFFSET(TILE_TABLE, FORMAT);
IFE_CHECK_OFFSET(TILE_TABLE, CIPHER_OFFSET);
IFE_CHECK_OFFSET(TILE_TABLE, TILE_OFFSETS_OFFSET);
IFE_CHECK_OFFSET(TILE_TABLE, LAYER_EXTENTS_OFFSET);
IFE_CHECK_OFFSET(TILE_TABLE, X_EXTENT);
IFE_CHECK_OFFSET(TILE_TABLE, Y_EXTENT);
IFE_CHECK_HEADER_SIZE(TILE_TABLE);
IFE_CHECK_RECOVERY(TILE_TABLE);

// ---- METADATA ----
IFE_CHECK_OFFSET(METADATA, VALIDATION);
IFE_CHECK_OFFSET(METADATA, RECOVERY);
IFE_CHECK_OFFSET(METADATA, CODEC_MAJOR);
IFE_CHECK_OFFSET(METADATA, CODEC_MINOR);
IFE_CHECK_OFFSET(METADATA, CODEC_BUILD);
IFE_CHECK_OFFSET(METADATA, ATTRIBUTES_OFFSET);
IFE_CHECK_OFFSET(METADATA, IMAGES_OFFSET);
IFE_CHECK_OFFSET(METADATA, ICC_COLOR_OFFSET);
IFE_CHECK_OFFSET(METADATA, ANNOTATIONS_OFFSET);
IFE_CHECK_OFFSET(METADATA, MICRONS_PIXEL);
IFE_CHECK_OFFSET(METADATA, MAGNIFICATION);
IFE_CHECK_HEADER_SIZE(METADATA);
IFE_CHECK_RECOVERY(METADATA);

// ---- ATTRIBUTES ----
IFE_CHECK_OFFSET(ATTRIBUTES, VALIDATION);
IFE_CHECK_OFFSET(ATTRIBUTES, RECOVERY);
IFE_CHECK_OFFSET(ATTRIBUTES, FORMAT);
IFE_CHECK_OFFSET(ATTRIBUTES, VERSION);
IFE_CHECK_OFFSET(ATTRIBUTES, LENGTHS_OFFSET);
IFE_CHECK_OFFSET(ATTRIBUTES, BYTE_ARRAY_OFFSET);
IFE_CHECK_HEADER_SIZE(ATTRIBUTES);
IFE_CHECK_RECOVERY(ATTRIBUTES);

// ---- LAYER_EXTENTS ----
IFE_CHECK_OFFSET(LAYER_EXTENTS, VALIDATION);
IFE_CHECK_OFFSET(LAYER_EXTENTS, RECOVERY);
IFE_CHECK_OFFSET(LAYER_EXTENTS, ENTRY_SIZE);
IFE_CHECK_OFFSET(LAYER_EXTENTS, ENTRY_NUMBER);
IFE_CHECK_HEADER_SIZE(LAYER_EXTENTS);
IFE_CHECK_RECOVERY(LAYER_EXTENTS);

// ---- TILE_OFFSETS ----
IFE_CHECK_OFFSET(TILE_OFFSETS, VALIDATION);
IFE_CHECK_OFFSET(TILE_OFFSETS, RECOVERY);
IFE_CHECK_OFFSET(TILE_OFFSETS, ENTRY_SIZE);
IFE_CHECK_OFFSET(TILE_OFFSETS, ENTRY_NUMBER);
IFE_CHECK_HEADER_SIZE(TILE_OFFSETS);
IFE_CHECK_RECOVERY(TILE_OFFSETS);

// ---- ATTRIBUTES_SIZES ----
IFE_CHECK_OFFSET(ATTRIBUTES_SIZES, VALIDATION);
IFE_CHECK_OFFSET(ATTRIBUTES_SIZES, RECOVERY);
IFE_CHECK_OFFSET(ATTRIBUTES_SIZES, ENTRY_SIZE);
IFE_CHECK_OFFSET(ATTRIBUTES_SIZES, ENTRY_NUMBER);
IFE_CHECK_HEADER_SIZE(ATTRIBUTES_SIZES);
IFE_CHECK_RECOVERY(ATTRIBUTES_SIZES);

// ---- ATTRIBUTES_BYTES ----
IFE_CHECK_OFFSET(ATTRIBUTES_BYTES, VALIDATION);
IFE_CHECK_OFFSET(ATTRIBUTES_BYTES, RECOVERY);
IFE_CHECK_OFFSET(ATTRIBUTES_BYTES, ENTRY_NUMBER);
IFE_CHECK_HEADER_SIZE(ATTRIBUTES_BYTES);
IFE_CHECK_RECOVERY(ATTRIBUTES_BYTES);

// ---- IMAGE_ARRAY ----
IFE_CHECK_OFFSET(IMAGE_ARRAY, VALIDATION);
IFE_CHECK_OFFSET(IMAGE_ARRAY, RECOVERY);
IFE_CHECK_OFFSET(IMAGE_ARRAY, ENTRY_SIZE);
IFE_CHECK_OFFSET(IMAGE_ARRAY, ENTRY_NUMBER);
IFE_CHECK_HEADER_SIZE(IMAGE_ARRAY);
IFE_CHECK_RECOVERY(IMAGE_ARRAY);

// ---- IMAGE_BYTES ----
IFE_CHECK_OFFSET(IMAGE_BYTES, VALIDATION);
IFE_CHECK_OFFSET(IMAGE_BYTES, RECOVERY);
IFE_CHECK_OFFSET(IMAGE_BYTES, TITLE_SIZE);
IFE_CHECK_OFFSET(IMAGE_BYTES, IMAGE_SIZE);
IFE_CHECK_HEADER_SIZE(IMAGE_BYTES);
IFE_CHECK_RECOVERY(IMAGE_BYTES);

// ---- ICC_PROFILE ----
IFE_CHECK_OFFSET(ICC_PROFILE, VALIDATION);
IFE_CHECK_OFFSET(ICC_PROFILE, RECOVERY);
IFE_CHECK_OFFSET(ICC_PROFILE, ENTRY_NUMBER);
IFE_CHECK_HEADER_SIZE(ICC_PROFILE);
IFE_CHECK_RECOVERY(ICC_PROFILE);

// ---- ANNOTATIONS ----
IFE_CHECK_OFFSET(ANNOTATIONS, VALIDATION);
IFE_CHECK_OFFSET(ANNOTATIONS, RECOVERY);
IFE_CHECK_OFFSET(ANNOTATIONS, ENTRY_SIZE);
IFE_CHECK_OFFSET(ANNOTATIONS, ENTRY_NUMBER);
IFE_CHECK_OFFSET(ANNOTATIONS, GROUP_SIZES_OFFSET);
IFE_CHECK_OFFSET(ANNOTATIONS, GROUP_BYTES_OFFSET);
IFE_CHECK_HEADER_SIZE(ANNOTATIONS);
IFE_CHECK_RECOVERY(ANNOTATIONS);

// ---- ANNOTATION_BYTES ----
IFE_CHECK_OFFSET(ANNOTATION_BYTES, VALIDATION);
IFE_CHECK_OFFSET(ANNOTATION_BYTES, RECOVERY);
IFE_CHECK_OFFSET(ANNOTATION_BYTES, ENTRY_NUMBER);
IFE_CHECK_HEADER_SIZE(ANNOTATION_BYTES);
IFE_CHECK_RECOVERY(ANNOTATION_BYTES);

// ---- ANNOTATION_GROUP_SIZES ----
IFE_CHECK_OFFSET(ANNOTATION_GROUP_SIZES, VALIDATION);
IFE_CHECK_OFFSET(ANNOTATION_GROUP_SIZES, RECOVERY);
IFE_CHECK_OFFSET(ANNOTATION_GROUP_SIZES, ENTRY_SIZE);
IFE_CHECK_OFFSET(ANNOTATION_GROUP_SIZES, ENTRY_NUMBER);
IFE_CHECK_HEADER_SIZE(ANNOTATION_GROUP_SIZES);
IFE_CHECK_RECOVERY(ANNOTATION_GROUP_SIZES);

// ---- ANNOTATION_GROUP_BYTES ----
IFE_CHECK_OFFSET(ANNOTATION_GROUP_BYTES, VALIDATION);
IFE_CHECK_OFFSET(ANNOTATION_GROUP_BYTES, RECOVERY);
IFE_CHECK_OFFSET(ANNOTATION_GROUP_BYTES, ENTRY_NUMBER);
IFE_CHECK_HEADER_SIZE(ANNOTATION_GROUP_BYTES);
IFE_CHECK_RECOVERY(ANNOTATION_GROUP_BYTES);

// -----------------------------------------------------------------------------
// Sub-block (datatype) wire-size parity. The generated `IFE::datatypes::*`
// PODs are reflective and may widen narrow on-disk types (uint24/uint40),
// so sizeof(struct) is NOT pinned — only the `wire_size` constant.
// -----------------------------------------------------------------------------
static_assert(GD::LAYER_EXTENT::wire_size  == L::LAYER_EXTENT::SIZE,
              "datatype LAYER_EXTENT wire_size drift");
static_assert(GD::TILE_OFFSET::wire_size   == L::TILE_OFFSET::SIZE,
              "datatype TILE_OFFSET wire_size drift");
static_assert(GD::ATTRIBUTE_SIZE::wire_size == L::ATTRIBUTE_SIZE::SIZE,
              "datatype ATTRIBUTE_SIZE wire_size drift");
static_assert(GD::IMAGE_ENTRY::wire_size   == L::IMAGE_ENTRY::SIZE,
              "datatype IMAGE_ENTRY wire_size drift");
static_assert(GD::ANNOTATION_ENTRY::wire_size       == L::ANNOTATION_ENTRY::SIZE,
              "datatype ANNOTATION_ENTRY wire_size drift");
static_assert(GD::ANNOTATION_GROUP_SIZE::wire_size  == L::ANNOTATION_GROUP_SIZE::SIZE,
              "datatype ANNOTATION_GROUP_SIZE wire_size drift");

// LAYER_EXTENT happens to have only natural-aligned 32-bit fields, so its
// in-memory layout matches the wire format byte-for-byte. The other
// datatypes do not (uint24/uint40 widen) so we don't pin sizeof for them.
static_assert(sizeof(GD::LAYER_EXTENT) == GD::LAYER_EXTENT::wire_size,
              "LAYER_EXTENT in-memory layout diverged from its wire size");

// -----------------------------------------------------------------------------
// FieldKey table — sanity checks on a representative sample. Most invariants
// (offsets/sizes per field) are already pinned by the IFE_CHECK_OFFSET /
// IFE_CHECK_SIZE static_asserts above; here we additionally verify the
// reflective FieldKey table agrees with those numbers for FILE_HEADER.
// -----------------------------------------------------------------------------
static_assert(GK::FILE_HEADER::fields.size() == 8,
              "FILE_HEADER FieldKey table size drift");
static_assert(GK::FILE_HEADER::fields[0].name == "MAGIC_BYTES_OFFSET");
static_assert(GK::FILE_HEADER::fields[0].offset == G::FILE_HEADER::offset::MAGIC_BYTES_OFFSET);
static_assert(GK::FILE_HEADER::fields[0].size   == G::FILE_HEADER::size::MAGIC_BYTES_OFFSET);
static_assert(GK::FILE_HEADER::fields[0].kind   == ::IFE::IFE_FieldKind::SCALAR);
static_assert(GK::FILE_HEADER::fields[0].scalar_tag == ::IFE::RECOVERY_TAG::SCALAR_UINT32);

// -----------------------------------------------------------------------------
// Schema version pinning. The codegen embeds the schema version as
// constexpr globals so external translation units that have been compiled
// against an older schema fail to link against a newer one.
// -----------------------------------------------------------------------------
static_assert(::IFE::IFE_SCHEMA_VERSION_MAJOR == 1, "schema version_major drift");
static_assert(::IFE::IFE_SCHEMA_VERSION_MINOR == 0, "schema version_minor drift");

int main() {
    // All real checks are static_assert-driven above. The runtime body
    // just confirms the binary was built and the FieldKey table is
    // populated as expected (catches accidental empty arrays).
    if (GK::FILE_HEADER::fields.empty()) {
        std::fprintf(stderr, "FAIL: FILE_HEADER FieldKey table is empty\n");
        return 1;
    }
    std::fprintf(stderr, "ife_codegen_tests: OK\n");
    return 0;
}
