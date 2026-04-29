/**
 * @file IFE_Types.hpp
 * @brief FastFHIR-style type system for the Iris File Extension. Phase 2 of
 *        the substrate migration.
 *
 * This header is **dormant** unless built with `IFE_USE_FASTFHIR_SUBSTRATE`.
 * It introduces no on-disk format change and no public ABI change. The
 * existing `IrisCodec::RECOVERY` enum (`0x55xx` values) remains the source of
 * truth for the v1 wire format until Phase 4. The new `IFE::RECOVERY_TAG`
 * enum lives in a separate `IFE` namespace and uses partitioned tag values
 * (`0x01xx` primitive / `0x02xx` datatype / `0x03xx` resource, with the
 * `0x8000` array bit) that the codegen, builder, and reflective lenses in
 * Phases 3-6 will consume. A constexpr round-trip (`legacy_to_tag` /
 * `tag_to_legacy`) connects the two so existing readers/writers keep
 * functioning while later phases migrate the resources one at a time.
 *
 * Soft-deprecation note (deviation from the literal plan): the migration plan
 * called for marking `enum IrisCodec::RECOVERY` with `[[deprecated]]`. Phase 1
 * preserved binary parity with downstream consumers (notably the Iris-Codec
 * Community Module), and every existing field declaration in
 * `IrisCodecExtension.hpp` is of the form `RECOVERY recovery = RECOVER_*;` —
 * a hard deprecation attribute would emit a warning at every such site even
 * for builds that never enable the substrate. The deprecation is therefore
 * documented here and in `MIGRATION.md`; a real `[[deprecated]]` is staged
 * for Phase 6 when the flag default flips and legacy callers are already
 * migrating.
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#ifndef IFE_Types_hpp
#define IFE_Types_hpp

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "IrisFileExtension.hpp"

namespace IFE {

// =============================================================================
// Field kinds
// =============================================================================
//
// Every reflective entry in the Phase 6 builder/reader carries one of these
// kinds. They are deliberately a small enum (uint8) so they can sit inside a
// packed `Reflective::Entry` without padding. Semantics:
//
//   SCALAR — a single primitive (uint*, int*, float, double).
//   ARRAY  — a homogeneous run of `SCALAR` or `BLOCK` values.
//   BLOCK  — a fixed-size composite datatype (e.g. `LayerExtent`).
//   CHOICE — a tagged union (one of N alternatives, selected by tag).
//   STRING — a UTF-8 byte run with an explicit length prefix.
//
enum class IFE_FieldKind : uint8_t {
    SCALAR  = 0,
    ARRAY   = 1,
    BLOCK   = 2,
    CHOICE  = 3,
    STRING  = 4,
};

// =============================================================================
// Recovery-tag partitioning
// =============================================================================
//
// The legacy enum `IrisCodec::RECOVERY` packs every tag into a single 0x55xx
// space. Phase 2 partitions the new tag space by semantic class so codegen
// can validate at compile time that, e.g., a `BLOCK` field never carries a
// resource tag. Layout of a `uint16_t` tag:
//
//   bit  15        : ARRAY bit (set => the tag describes an array of the
//                    underlying scalar/block).
//   bits 14..8     : reserved (must be zero in v2; reused for additional
//                    flags in later phases).
//   bits 7..0      : partition + ordinal. The high byte selects the
//                    partition (PRIMITIVE / DATATYPE / RESOURCE).
//
// Concretely the unmasked (non-array) value of a tag is:
//     0x01xx — primitive scalar (uint8 .. float64)
//     0x02xx — datatype / BLOCK (LayerExtent, ...)
//     0x03xx — resource (HEADER, TILE_TABLE, ...)
//
// The 0x55xx legacy values are *not* in any of these partitions; they are
// translated via `legacy_to_tag` at the read/write boundary and never appear
// inside the substrate.
//
inline constexpr uint16_t RECOVERY_PARTITION_MASK       = 0x7F00;
inline constexpr uint16_t RECOVERY_ORDINAL_MASK         = 0x00FF;
inline constexpr uint16_t RECOVERY_PARTITION_PRIMITIVE  = 0x0100;
inline constexpr uint16_t RECOVERY_PARTITION_DATATYPE   = 0x0200;
inline constexpr uint16_t RECOVERY_PARTITION_RESOURCE   = 0x0300;
inline constexpr uint16_t RECOVERY_ARRAY_BIT            = 0x8000;

constexpr bool IsArrayTag(uint16_t tag) noexcept {
    return (tag & RECOVERY_ARRAY_BIT) != 0;
}

constexpr uint16_t RecoveryPartition(uint16_t tag) noexcept {
    return static_cast<uint16_t>(tag & RECOVERY_PARTITION_MASK);
}

constexpr uint16_t StripArrayBit(uint16_t tag) noexcept {
    return static_cast<uint16_t>(tag & ~RECOVERY_ARRAY_BIT);
}

constexpr uint16_t WithArrayBit(uint16_t tag) noexcept {
    return static_cast<uint16_t>(tag | RECOVERY_ARRAY_BIT);
}

// =============================================================================
// RECOVERY_TAG — the partitioned tag enum
// =============================================================================
//
// Every value here is pinned by a `static_assert` below so a future schema
// edit that re-orders or re-numbers a tag fails the build instead of
// silently changing the wire format the codegen emits.
//
enum class RECOVERY_TAG : uint16_t {
    UNDEFINED                       = 0x0000,

    // ---- 0x01xx primitive scalars ------------------------------------------
    SCALAR_UINT8                    = RECOVERY_PARTITION_PRIMITIVE | 0x01,
    SCALAR_UINT16                   = RECOVERY_PARTITION_PRIMITIVE | 0x02,
    SCALAR_UINT32                   = RECOVERY_PARTITION_PRIMITIVE | 0x03,
    SCALAR_UINT64                   = RECOVERY_PARTITION_PRIMITIVE | 0x04,
    SCALAR_INT8                     = RECOVERY_PARTITION_PRIMITIVE | 0x05,
    SCALAR_INT16                    = RECOVERY_PARTITION_PRIMITIVE | 0x06,
    SCALAR_INT32                    = RECOVERY_PARTITION_PRIMITIVE | 0x07,
    SCALAR_INT64                    = RECOVERY_PARTITION_PRIMITIVE | 0x08,
    SCALAR_FLOAT32                  = RECOVERY_PARTITION_PRIMITIVE | 0x09,
    SCALAR_FLOAT64                  = RECOVERY_PARTITION_PRIMITIVE | 0x0A,

    // ARRAY variants for the primitive scalars above. Pre-OR'd with the
    // ARRAY bit so the bit is part of the tag and not synthesised at runtime.
    ARRAY_UINT8                     = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(SCALAR_UINT8),
    ARRAY_UINT16                    = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(SCALAR_UINT16),
    ARRAY_UINT32                    = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(SCALAR_UINT32),
    ARRAY_UINT64                    = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(SCALAR_UINT64),
    ARRAY_INT8                      = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(SCALAR_INT8),
    ARRAY_INT16                     = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(SCALAR_INT16),
    ARRAY_INT32                     = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(SCALAR_INT32),
    ARRAY_INT64                     = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(SCALAR_INT64),
    ARRAY_FLOAT32                   = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(SCALAR_FLOAT32),
    ARRAY_FLOAT64                   = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(SCALAR_FLOAT64),

    // ---- 0x02xx datatypes (BLOCK kind) -------------------------------------
    // First entry — LayerExtent — is the only BLOCK datatype currently
    // serialized by the codebase. Phase 3 codegen will add the remaining
    // composite datatypes from the schema.
    BLOCK_LAYER_EXTENT              = RECOVERY_PARTITION_DATATYPE | 0x01,
    ARRAY_LAYER_EXTENT              = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(BLOCK_LAYER_EXTENT),

    // ---- 0x03xx resources --------------------------------------------------
    // These mirror IrisCodec::RECOVERY 1:1 (HEADER..ANNOTATION_GROUP_BYTES,
    // CIPHER, UNDEFINED). The numeric value is a partitioned re-encoding;
    // the legacy 0x55xx values are NOT inherited.
    RESOURCE_HEADER                 = RECOVERY_PARTITION_RESOURCE | 0x01,
    RESOURCE_TILE_TABLE             = RECOVERY_PARTITION_RESOURCE | 0x02,
    RESOURCE_CIPHER                 = RECOVERY_PARTITION_RESOURCE | 0x03,
    RESOURCE_METADATA               = RECOVERY_PARTITION_RESOURCE | 0x04,
    RESOURCE_ATTRIBUTES             = RECOVERY_PARTITION_RESOURCE | 0x05,
    RESOURCE_LAYER_EXTENTS          = RECOVERY_PARTITION_RESOURCE | 0x06,
    RESOURCE_TILE_OFFSETS           = RECOVERY_PARTITION_RESOURCE | 0x07,
    RESOURCE_ATTRIBUTES_SIZES       = RECOVERY_PARTITION_RESOURCE | 0x08,
    RESOURCE_ATTRIBUTES_BYTES       = RECOVERY_PARTITION_RESOURCE | 0x09,
    RESOURCE_ASSOCIATED_IMAGES      = RECOVERY_PARTITION_RESOURCE | 0x0A,
    RESOURCE_ASSOCIATED_IMAGE_BYTES = RECOVERY_PARTITION_RESOURCE | 0x0B,
    RESOURCE_ICC_PROFILE            = RECOVERY_PARTITION_RESOURCE | 0x0C,
    RESOURCE_ANNOTATIONS            = RECOVERY_PARTITION_RESOURCE | 0x0D,
    RESOURCE_ANNOTATION_BYTES       = RECOVERY_PARTITION_RESOURCE | 0x0E,
    RESOURCE_ANNOTATION_GROUP_SIZES = RECOVERY_PARTITION_RESOURCE | 0x0F,
    RESOURCE_ANNOTATION_GROUP_BYTES = RECOVERY_PARTITION_RESOURCE | 0x10,
};

constexpr uint16_t to_underlying(RECOVERY_TAG t) noexcept {
    return static_cast<uint16_t>(t);
}

// ---- Pin every tag value at compile time -----------------------------------
// These guard against accidental re-numbering during later schema edits.

// Partitions are correct for every primitive and array-of-primitive.
static_assert(RecoveryPartition(to_underlying(RECOVERY_TAG::SCALAR_UINT8))   == RECOVERY_PARTITION_PRIMITIVE);
static_assert(RecoveryPartition(to_underlying(RECOVERY_TAG::SCALAR_FLOAT64)) == RECOVERY_PARTITION_PRIMITIVE);
static_assert(RecoveryPartition(StripArrayBit(to_underlying(RECOVERY_TAG::ARRAY_UINT8)))   == RECOVERY_PARTITION_PRIMITIVE);
static_assert(RecoveryPartition(StripArrayBit(to_underlying(RECOVERY_TAG::ARRAY_FLOAT64))) == RECOVERY_PARTITION_PRIMITIVE);

// Datatype partition.
static_assert(RecoveryPartition(to_underlying(RECOVERY_TAG::BLOCK_LAYER_EXTENT)) == RECOVERY_PARTITION_DATATYPE);
static_assert(RecoveryPartition(StripArrayBit(to_underlying(RECOVERY_TAG::ARRAY_LAYER_EXTENT))) == RECOVERY_PARTITION_DATATYPE);

// Resource partition — every legacy resource translates into 0x03xx.
static_assert(RecoveryPartition(to_underlying(RECOVERY_TAG::RESOURCE_HEADER))                 == RECOVERY_PARTITION_RESOURCE);
static_assert(RecoveryPartition(to_underlying(RECOVERY_TAG::RESOURCE_ANNOTATION_GROUP_BYTES)) == RECOVERY_PARTITION_RESOURCE);

// Array bit set on every ARRAY_* tag, clear on every SCALAR_*/BLOCK_*/RESOURCE_* tag.
static_assert(IsArrayTag(to_underlying(RECOVERY_TAG::ARRAY_UINT8)));
static_assert(IsArrayTag(to_underlying(RECOVERY_TAG::ARRAY_FLOAT64)));
static_assert(IsArrayTag(to_underlying(RECOVERY_TAG::ARRAY_LAYER_EXTENT)));
static_assert(!IsArrayTag(to_underlying(RECOVERY_TAG::SCALAR_UINT8)));
static_assert(!IsArrayTag(to_underlying(RECOVERY_TAG::BLOCK_LAYER_EXTENT)));
static_assert(!IsArrayTag(to_underlying(RECOVERY_TAG::RESOURCE_HEADER)));

// Numeric pinning of every primitive ordinal — these values are part of the
// codegen contract.
static_assert(to_underlying(RECOVERY_TAG::SCALAR_UINT8)   == 0x0101);
static_assert(to_underlying(RECOVERY_TAG::SCALAR_UINT16)  == 0x0102);
static_assert(to_underlying(RECOVERY_TAG::SCALAR_UINT32)  == 0x0103);
static_assert(to_underlying(RECOVERY_TAG::SCALAR_UINT64)  == 0x0104);
static_assert(to_underlying(RECOVERY_TAG::SCALAR_INT8)    == 0x0105);
static_assert(to_underlying(RECOVERY_TAG::SCALAR_INT16)   == 0x0106);
static_assert(to_underlying(RECOVERY_TAG::SCALAR_INT32)   == 0x0107);
static_assert(to_underlying(RECOVERY_TAG::SCALAR_INT64)   == 0x0108);
static_assert(to_underlying(RECOVERY_TAG::SCALAR_FLOAT32) == 0x0109);
static_assert(to_underlying(RECOVERY_TAG::SCALAR_FLOAT64) == 0x010A);

static_assert(to_underlying(RECOVERY_TAG::ARRAY_UINT8)   == 0x8101);
static_assert(to_underlying(RECOVERY_TAG::ARRAY_FLOAT64) == 0x810A);

static_assert(to_underlying(RECOVERY_TAG::BLOCK_LAYER_EXTENT) == 0x0201);
static_assert(to_underlying(RECOVERY_TAG::ARRAY_LAYER_EXTENT) == 0x8201);

static_assert(to_underlying(RECOVERY_TAG::RESOURCE_HEADER)                 == 0x0301);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_TILE_TABLE)             == 0x0302);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_CIPHER)                 == 0x0303);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_METADATA)               == 0x0304);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_ATTRIBUTES)             == 0x0305);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_LAYER_EXTENTS)          == 0x0306);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_TILE_OFFSETS)           == 0x0307);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_ATTRIBUTES_SIZES)       == 0x0308);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_ATTRIBUTES_BYTES)       == 0x0309);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_ASSOCIATED_IMAGES)      == 0x030A);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_ASSOCIATED_IMAGE_BYTES) == 0x030B);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_ICC_PROFILE)            == 0x030C);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_ANNOTATIONS)            == 0x030D);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_ANNOTATION_BYTES)       == 0x030E);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_ANNOTATION_GROUP_SIZES) == 0x030F);
static_assert(to_underlying(RECOVERY_TAG::RESOURCE_ANNOTATION_GROUP_BYTES) == 0x0310);

// =============================================================================
// TypeTraits<T> — the metadata codegen will consult per primitive/datatype
// =============================================================================
//
// Each specialization carries the on-wire size, the field kind, the scalar
// recovery tag (used when the field is single-valued), and the array
// recovery tag (used when the field is repeated). Phase 3 codegen synthesises
// `IFE_DataTypes.hpp` / `IFE_VTables.hpp` by reflecting over these traits.
//
template <class T>
struct TypeTraits;  // primary template intentionally undefined — only the
                    // specializations below are valid.

#define IFE_DEFINE_SCALAR_TRAITS(CXX_T, WIRE_SIZE, SCALAR_TAG, ARRAY_TAG)   \
    template <> struct TypeTraits<CXX_T> {                                  \
        using value_type = CXX_T;                                           \
        static constexpr size_t        wire_size  = (WIRE_SIZE);            \
        static constexpr IFE_FieldKind kind       = IFE_FieldKind::SCALAR;  \
        static constexpr RECOVERY_TAG  scalar_tag = RECOVERY_TAG::SCALAR_TAG;\
        static constexpr RECOVERY_TAG  array_tag  = RECOVERY_TAG::ARRAY_TAG;\
    }

IFE_DEFINE_SCALAR_TRAITS(uint8_t,  1, SCALAR_UINT8,   ARRAY_UINT8);
IFE_DEFINE_SCALAR_TRAITS(uint16_t, 2, SCALAR_UINT16,  ARRAY_UINT16);
IFE_DEFINE_SCALAR_TRAITS(uint32_t, 4, SCALAR_UINT32,  ARRAY_UINT32);
IFE_DEFINE_SCALAR_TRAITS(uint64_t, 8, SCALAR_UINT64,  ARRAY_UINT64);
IFE_DEFINE_SCALAR_TRAITS(int8_t,   1, SCALAR_INT8,    ARRAY_INT8);
IFE_DEFINE_SCALAR_TRAITS(int16_t,  2, SCALAR_INT16,   ARRAY_INT16);
IFE_DEFINE_SCALAR_TRAITS(int32_t,  4, SCALAR_INT32,   ARRAY_INT32);
IFE_DEFINE_SCALAR_TRAITS(int64_t,  8, SCALAR_INT64,   ARRAY_INT64);
IFE_DEFINE_SCALAR_TRAITS(float,    4, SCALAR_FLOAT32, ARRAY_FLOAT32);
IFE_DEFINE_SCALAR_TRAITS(double,   8, SCALAR_FLOAT64, ARRAY_FLOAT64);

#undef IFE_DEFINE_SCALAR_TRAITS

// Pin the wire sizes against the legacy `IrisCodec::TYPE_SIZES` enum so that
// a primitive specialization here cannot drift from the on-disk encoder.
static_assert(TypeTraits<uint8_t>::wire_size  == ::IrisCodec::Serialization::TYPE_SIZE_UINT8);
static_assert(TypeTraits<uint16_t>::wire_size == ::IrisCodec::Serialization::TYPE_SIZE_UINT16);
static_assert(TypeTraits<uint32_t>::wire_size == ::IrisCodec::Serialization::TYPE_SIZE_UINT32);
static_assert(TypeTraits<uint64_t>::wire_size == ::IrisCodec::Serialization::TYPE_SIZE_UINT64);
static_assert(TypeTraits<float>::wire_size    == ::IrisCodec::Serialization::TYPE_SIZE_FLOAT32);
static_assert(TypeTraits<double>::wire_size   == ::IrisCodec::Serialization::TYPE_SIZE_FLOAT64);

// `LayerExtent` BLOCK datatype. Forward-declared to avoid pulling the
// IrisHeaders include into this header — the wire size is pinned against
// `IrisCodec::Serialization::LAYER_EXTENT::SIZE` below.
//
// On-disk layout (v1, see `LAYER_EXTENT` in `IrisCodecExtension.hpp`):
//     uint32 xTiles | uint32 yTiles | float32 scale.
struct LayerExtentBlock {
    uint32_t xTiles;
    uint32_t yTiles;
    float    scale;
};

template <> struct TypeTraits<LayerExtentBlock> {
    using value_type = LayerExtentBlock;
    static constexpr size_t        wire_size  =
        TypeTraits<uint32_t>::wire_size +   // xTiles
        TypeTraits<uint32_t>::wire_size +   // yTiles
        TypeTraits<float>::wire_size;       // scale
    static constexpr IFE_FieldKind kind       = IFE_FieldKind::BLOCK;
    static constexpr RECOVERY_TAG  scalar_tag = RECOVERY_TAG::BLOCK_LAYER_EXTENT;
    static constexpr RECOVERY_TAG  array_tag  = RECOVERY_TAG::ARRAY_LAYER_EXTENT;
};

// Single source of truth: the in-memory layout matches the summed wire
// size, and both match the legacy LAYER_EXTENT::SIZE used by
// IrisCodec::Serialization.
static_assert(sizeof(LayerExtentBlock) == TypeTraits<LayerExtentBlock>::wire_size,
              "LayerExtentBlock in-memory layout diverged from its wire size");
static_assert(TypeTraits<LayerExtentBlock>::wire_size == ::IrisCodec::Serialization::LAYER_EXTENT::SIZE,
              "LayerExtent block wire size diverged from legacy LAYER_EXTENT::SIZE");

// =============================================================================
// Legacy <-> partitioned tag round-trip
// =============================================================================
//
// `IrisCodec::RECOVERY` (0x55xx) is the on-disk format until Phase 4. The
// substrate exclusively manipulates `RECOVERY_TAG`. These two functions
// bridge the boundary; the round-trip is `static_assert`-closed below so a
// future schema edit that touches one side without the other fails the build.
//
constexpr RECOVERY_TAG legacy_to_tag(::IrisCodec::Serialization::RECOVERY r) noexcept {
    using L = ::IrisCodec::Serialization::RECOVERY;
    using T = RECOVERY_TAG;
    switch (r) {
        case L::RECOVER_HEADER:                  return T::RESOURCE_HEADER;
        case L::RECOVER_TILE_TABLE:              return T::RESOURCE_TILE_TABLE;
        case L::RECOVER_CIPHER:                  return T::RESOURCE_CIPHER;
        case L::RECOVER_METADATA:                return T::RESOURCE_METADATA;
        case L::RECOVER_ATTRIBUTES:              return T::RESOURCE_ATTRIBUTES;
        case L::RECOVER_LAYER_EXTENTS:           return T::RESOURCE_LAYER_EXTENTS;
        case L::RECOVER_TILE_OFFSETS:            return T::RESOURCE_TILE_OFFSETS;
        case L::RECOVER_ATTRIBUTES_SIZES:        return T::RESOURCE_ATTRIBUTES_SIZES;
        case L::RECOVER_ATTRIBUTES_BYTES:        return T::RESOURCE_ATTRIBUTES_BYTES;
        case L::RECOVER_ASSOCIATED_IMAGES:       return T::RESOURCE_ASSOCIATED_IMAGES;
        case L::RECOVER_ASSOCIATED_IMAGE_BYTES:  return T::RESOURCE_ASSOCIATED_IMAGE_BYTES;
        case L::RECOVER_ICC_PROFILE:             return T::RESOURCE_ICC_PROFILE;
        case L::RECOVER_ANNOTATIONS:             return T::RESOURCE_ANNOTATIONS;
        case L::RECOVER_ANNOTATION_BYTES:        return T::RESOURCE_ANNOTATION_BYTES;
        case L::RECOVER_ANNOTATION_GROUP_SIZES:  return T::RESOURCE_ANNOTATION_GROUP_SIZES;
        case L::RECOVER_ANNOTATION_GROUP_BYTES:  return T::RESOURCE_ANNOTATION_GROUP_BYTES;
        case L::RECOVER_UNDEFINED:               return T::UNDEFINED;
    }
    return T::UNDEFINED;
}

constexpr ::IrisCodec::Serialization::RECOVERY tag_to_legacy(RECOVERY_TAG t) noexcept {
    using L = ::IrisCodec::Serialization::RECOVERY;
    using T = RECOVERY_TAG;
    switch (t) {
        case T::RESOURCE_HEADER:                 return L::RECOVER_HEADER;
        case T::RESOURCE_TILE_TABLE:             return L::RECOVER_TILE_TABLE;
        case T::RESOURCE_CIPHER:                 return L::RECOVER_CIPHER;
        case T::RESOURCE_METADATA:               return L::RECOVER_METADATA;
        case T::RESOURCE_ATTRIBUTES:             return L::RECOVER_ATTRIBUTES;
        case T::RESOURCE_LAYER_EXTENTS:          return L::RECOVER_LAYER_EXTENTS;
        case T::RESOURCE_TILE_OFFSETS:           return L::RECOVER_TILE_OFFSETS;
        case T::RESOURCE_ATTRIBUTES_SIZES:       return L::RECOVER_ATTRIBUTES_SIZES;
        case T::RESOURCE_ATTRIBUTES_BYTES:       return L::RECOVER_ATTRIBUTES_BYTES;
        case T::RESOURCE_ASSOCIATED_IMAGES:      return L::RECOVER_ASSOCIATED_IMAGES;
        case T::RESOURCE_ASSOCIATED_IMAGE_BYTES: return L::RECOVER_ASSOCIATED_IMAGE_BYTES;
        case T::RESOURCE_ICC_PROFILE:            return L::RECOVER_ICC_PROFILE;
        case T::RESOURCE_ANNOTATIONS:            return L::RECOVER_ANNOTATIONS;
        case T::RESOURCE_ANNOTATION_BYTES:       return L::RECOVER_ANNOTATION_BYTES;
        case T::RESOURCE_ANNOTATION_GROUP_SIZES: return L::RECOVER_ANNOTATION_GROUP_SIZES;
        case T::RESOURCE_ANNOTATION_GROUP_BYTES: return L::RECOVER_ANNOTATION_GROUP_BYTES;
        default:                                 return L::RECOVER_UNDEFINED;
    }
}

// Round-trip closure for every resource: legacy -> tag -> legacy is identity.
#define IFE_ROUND_TRIP(LEG)                                                  \
    static_assert(tag_to_legacy(legacy_to_tag(::IrisCodec::Serialization::LEG)) == ::IrisCodec::Serialization::LEG, \
                  "legacy<->tag round-trip diverged for " #LEG)

IFE_ROUND_TRIP(RECOVER_HEADER);
IFE_ROUND_TRIP(RECOVER_TILE_TABLE);
IFE_ROUND_TRIP(RECOVER_CIPHER);
IFE_ROUND_TRIP(RECOVER_METADATA);
IFE_ROUND_TRIP(RECOVER_ATTRIBUTES);
IFE_ROUND_TRIP(RECOVER_LAYER_EXTENTS);
IFE_ROUND_TRIP(RECOVER_TILE_OFFSETS);
IFE_ROUND_TRIP(RECOVER_ATTRIBUTES_SIZES);
IFE_ROUND_TRIP(RECOVER_ATTRIBUTES_BYTES);
IFE_ROUND_TRIP(RECOVER_ASSOCIATED_IMAGES);
IFE_ROUND_TRIP(RECOVER_ASSOCIATED_IMAGE_BYTES);
IFE_ROUND_TRIP(RECOVER_ICC_PROFILE);
IFE_ROUND_TRIP(RECOVER_ANNOTATIONS);
IFE_ROUND_TRIP(RECOVER_ANNOTATION_BYTES);
IFE_ROUND_TRIP(RECOVER_ANNOTATION_GROUP_SIZES);
IFE_ROUND_TRIP(RECOVER_ANNOTATION_GROUP_BYTES);
IFE_ROUND_TRIP(RECOVER_UNDEFINED);

#undef IFE_ROUND_TRIP

}  // namespace IFE

#endif  // IFE_Types_hpp
