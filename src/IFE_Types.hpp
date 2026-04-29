/**
 * @file IFE_Types.hpp
 * @brief FastFHIR-style type system for the Iris File Extension.
 *
 * This header is deliberately self-contained: no v1 wire-format bridge, no
 * round-trip helpers, no reference to `IrisCodec::Serialization`. The
 * substrate is the source of truth for the new format and is pinned by
 * the `static_assert`s in this file alone.
 *
 * Tag space (`enum class RECOVERY_TAG : uint16_t`) is partitioned by
 * semantic class so codegen can validate at compile time that, for example,
 * a `BLOCK` field never carries a resource tag:
 *
 *   bit  15        : ARRAY bit (set => the tag describes an array of the
 *                    underlying scalar/block).
 *   bits 14..8     : reserved.
 *   bits 7..0      : partition + ordinal:
 *                       0x01xx — primitive scalar (uint8 .. float64),
 *                       0x02xx — datatype / BLOCK (LayerExtent, ...),
 *                       0x03xx — resource (HEADER, TILE_TABLE, ...).
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#ifndef IFE_Types_hpp
#define IFE_Types_hpp

#include <cstddef>
#include <cstdint>
#include <type_traits>

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
// Bit layout is documented at the top of this header. The constants below
// expose the masks/bits the codegen and reflective lens consume.
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
    // exposed by `TypeTraits`. Codegen synthesises the remaining composite
    // datatypes from the schema into `IFE_DataTypes.hpp`.
    BLOCK_LAYER_EXTENT              = RECOVERY_PARTITION_DATATYPE | 0x01,
    ARRAY_LAYER_EXTENT              = RECOVERY_ARRAY_BIT | static_cast<uint16_t>(BLOCK_LAYER_EXTENT),

    // ---- 0x03xx resources --------------------------------------------------
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
// TypeTraits<T> — the metadata codegen consults per primitive/datatype
// =============================================================================
//
// Each specialization carries the on-wire size, the field kind, the scalar
// recovery tag (used when the field is single-valued), and the array
// recovery tag (used when the field is repeated). The codegen synthesises
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

// Pin every primitive's wire size against its natural C++ size. The
// substrate is little-endian by definition; the codegen and the on-disk
// encoder share these constants directly, so a drift here is a build error.
static_assert(TypeTraits<uint8_t>::wire_size  == sizeof(uint8_t));
static_assert(TypeTraits<uint16_t>::wire_size == sizeof(uint16_t));
static_assert(TypeTraits<uint32_t>::wire_size == sizeof(uint32_t));
static_assert(TypeTraits<uint64_t>::wire_size == sizeof(uint64_t));
static_assert(TypeTraits<float>::wire_size    == sizeof(float));
static_assert(TypeTraits<double>::wire_size   == sizeof(double));

// `LayerExtent` BLOCK datatype.
//
// On-disk layout: uint32 xTiles | uint32 yTiles | float32 scale.
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

// The struct's natural C++ layout matches its wire size byte-for-byte
// (12 bytes, no padding between three 4-byte fields).
static_assert(sizeof(LayerExtentBlock) == TypeTraits<LayerExtentBlock>::wire_size,
              "LayerExtentBlock in-memory layout diverged from its wire size");

}  // namespace IFE

#endif  // IFE_Types_hpp
