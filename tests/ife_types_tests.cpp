/**
 * @file ife_types_tests.cpp
 * @brief Phase 2 tests for IFE_Types.hpp.
 *
 * The bulk of the type-system invariants are pinned at compile time via
 * `static_assert` inside `IFE_Types.hpp` itself; getting this translation
 * unit to compile *is* most of the test. This file additionally exercises
 * the legacy<->tag round-trip at runtime to guard against any future change
 * that makes the mapping non-`constexpr` or partial.
 *
 * Self-contained (no external test framework) so CI doesn't need extra deps.
 * Run with `ctest` or directly; non-zero exit on failure.
 */
#include "IFE_Types.hpp"

#include <cstdio>
#include <cstdint>

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do {                                            \
    if (!(cond)) {                                                      \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n",                      \
                     #cond, __FILE__, __LINE__);                        \
        ++g_failures;                                                   \
    }                                                                   \
} while (0)

// Compile-time round-trip is already verified inside IFE_Types.hpp; this
// table-driven runtime test additionally covers the iteration path that
// the Phase 6 builder/reader will use when walking a v1 file.
struct LegacyMapping {
    ::IrisCodec::Serialization::RECOVERY legacy;
    IFE::RECOVERY_TAG     tag;
    const char*           name;
};

constexpr LegacyMapping kMappings[] = {
    {::IrisCodec::Serialization::RECOVER_HEADER,                 IFE::RECOVERY_TAG::RESOURCE_HEADER,                 "HEADER"},
    {::IrisCodec::Serialization::RECOVER_TILE_TABLE,             IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE,             "TILE_TABLE"},
    {::IrisCodec::Serialization::RECOVER_CIPHER,                 IFE::RECOVERY_TAG::RESOURCE_CIPHER,                 "CIPHER"},
    {::IrisCodec::Serialization::RECOVER_METADATA,               IFE::RECOVERY_TAG::RESOURCE_METADATA,               "METADATA"},
    {::IrisCodec::Serialization::RECOVER_ATTRIBUTES,             IFE::RECOVERY_TAG::RESOURCE_ATTRIBUTES,             "ATTRIBUTES"},
    {::IrisCodec::Serialization::RECOVER_LAYER_EXTENTS,          IFE::RECOVERY_TAG::RESOURCE_LAYER_EXTENTS,          "LAYER_EXTENTS"},
    {::IrisCodec::Serialization::RECOVER_TILE_OFFSETS,           IFE::RECOVERY_TAG::RESOURCE_TILE_OFFSETS,           "TILE_OFFSETS"},
    {::IrisCodec::Serialization::RECOVER_ATTRIBUTES_SIZES,       IFE::RECOVERY_TAG::RESOURCE_ATTRIBUTES_SIZES,       "ATTRIBUTES_SIZES"},
    {::IrisCodec::Serialization::RECOVER_ATTRIBUTES_BYTES,       IFE::RECOVERY_TAG::RESOURCE_ATTRIBUTES_BYTES,       "ATTRIBUTES_BYTES"},
    {::IrisCodec::Serialization::RECOVER_ASSOCIATED_IMAGES,      IFE::RECOVERY_TAG::RESOURCE_ASSOCIATED_IMAGES,      "ASSOCIATED_IMAGES"},
    {::IrisCodec::Serialization::RECOVER_ASSOCIATED_IMAGE_BYTES, IFE::RECOVERY_TAG::RESOURCE_ASSOCIATED_IMAGE_BYTES, "ASSOCIATED_IMAGE_BYTES"},
    {::IrisCodec::Serialization::RECOVER_ICC_PROFILE,            IFE::RECOVERY_TAG::RESOURCE_ICC_PROFILE,            "ICC_PROFILE"},
    {::IrisCodec::Serialization::RECOVER_ANNOTATIONS,            IFE::RECOVERY_TAG::RESOURCE_ANNOTATIONS,            "ANNOTATIONS"},
    {::IrisCodec::Serialization::RECOVER_ANNOTATION_BYTES,       IFE::RECOVERY_TAG::RESOURCE_ANNOTATION_BYTES,       "ANNOTATION_BYTES"},
    {::IrisCodec::Serialization::RECOVER_ANNOTATION_GROUP_SIZES, IFE::RECOVERY_TAG::RESOURCE_ANNOTATION_GROUP_SIZES, "ANNOTATION_GROUP_SIZES"},
    {::IrisCodec::Serialization::RECOVER_ANNOTATION_GROUP_BYTES, IFE::RECOVERY_TAG::RESOURCE_ANNOTATION_GROUP_BYTES, "ANNOTATION_GROUP_BYTES"},
    {::IrisCodec::Serialization::RECOVER_UNDEFINED,              IFE::RECOVERY_TAG::UNDEFINED,                       "UNDEFINED"},
};

void test_legacy_round_trip() {
    for (const auto& m : kMappings) {
        const auto fwd = IFE::legacy_to_tag(m.legacy);
        const auto rev = IFE::tag_to_legacy(m.tag);
        if (fwd != m.tag) {
            std::fprintf(stderr,
                "FAIL: legacy_to_tag(%s)=0x%04x expected 0x%04x\n",
                m.name,
                static_cast<unsigned>(IFE::to_underlying(fwd)),
                static_cast<unsigned>(IFE::to_underlying(m.tag)));
            ++g_failures;
        }
        if (rev != m.legacy) {
            std::fprintf(stderr,
                "FAIL: tag_to_legacy(%s)=0x%04x expected 0x%04x\n",
                m.name,
                static_cast<unsigned>(rev),
                static_cast<unsigned>(m.legacy));
            ++g_failures;
        }
    }
}

void test_partition_helpers() {
    using IFE::IsArrayTag;
    using IFE::RecoveryPartition;
    using IFE::StripArrayBit;
    using IFE::WithArrayBit;
    using IFE::to_underlying;

    IFE_CHECK(!IsArrayTag(to_underlying(IFE::RECOVERY_TAG::SCALAR_UINT32)));
    IFE_CHECK( IsArrayTag(to_underlying(IFE::RECOVERY_TAG::ARRAY_UINT32)));
    IFE_CHECK( IsArrayTag(to_underlying(IFE::RECOVERY_TAG::ARRAY_LAYER_EXTENT)));

    // Stripping the array bit takes ARRAY_* back to its SCALAR/BLOCK base.
    IFE_CHECK(StripArrayBit(to_underlying(IFE::RECOVERY_TAG::ARRAY_UINT32)) ==
              to_underlying(IFE::RECOVERY_TAG::SCALAR_UINT32));
    IFE_CHECK(StripArrayBit(to_underlying(IFE::RECOVERY_TAG::ARRAY_LAYER_EXTENT)) ==
              to_underlying(IFE::RECOVERY_TAG::BLOCK_LAYER_EXTENT));

    IFE_CHECK(WithArrayBit(to_underlying(IFE::RECOVERY_TAG::SCALAR_UINT32)) ==
              to_underlying(IFE::RECOVERY_TAG::ARRAY_UINT32));

    IFE_CHECK(RecoveryPartition(to_underlying(IFE::RECOVERY_TAG::SCALAR_UINT32))
              == IFE::RECOVERY_PARTITION_PRIMITIVE);
    IFE_CHECK(RecoveryPartition(to_underlying(IFE::RECOVERY_TAG::BLOCK_LAYER_EXTENT))
              == IFE::RECOVERY_PARTITION_DATATYPE);
    IFE_CHECK(RecoveryPartition(to_underlying(IFE::RECOVERY_TAG::RESOURCE_HEADER))
              == IFE::RECOVERY_PARTITION_RESOURCE);
    // The array bit is outside the partition mask, so partition is invariant
    // under setting/clearing it.
    IFE_CHECK(RecoveryPartition(to_underlying(IFE::RECOVERY_TAG::ARRAY_UINT32))
              == IFE::RECOVERY_PARTITION_PRIMITIVE);
}

void test_type_traits() {
    // Wire sizes match the legacy IrisCodec::TYPE_SIZES table.
    IFE_CHECK(IFE::TypeTraits<uint8_t>::wire_size  == 1);
    IFE_CHECK(IFE::TypeTraits<uint16_t>::wire_size == 2);
    IFE_CHECK(IFE::TypeTraits<uint32_t>::wire_size == 4);
    IFE_CHECK(IFE::TypeTraits<uint64_t>::wire_size == 8);
    IFE_CHECK(IFE::TypeTraits<int8_t>::wire_size   == 1);
    IFE_CHECK(IFE::TypeTraits<int16_t>::wire_size  == 2);
    IFE_CHECK(IFE::TypeTraits<int32_t>::wire_size  == 4);
    IFE_CHECK(IFE::TypeTraits<int64_t>::wire_size  == 8);
    IFE_CHECK(IFE::TypeTraits<float>::wire_size    == 4);
    IFE_CHECK(IFE::TypeTraits<double>::wire_size   == 8);

    // SCALAR/ARRAY tag pairing is consistent.
    IFE_CHECK(IFE::TypeTraits<uint32_t>::scalar_tag == IFE::RECOVERY_TAG::SCALAR_UINT32);
    IFE_CHECK(IFE::TypeTraits<uint32_t>::array_tag  == IFE::RECOVERY_TAG::ARRAY_UINT32);
    IFE_CHECK(IFE::TypeTraits<float>::scalar_tag    == IFE::RECOVERY_TAG::SCALAR_FLOAT32);
    IFE_CHECK(IFE::TypeTraits<float>::array_tag     == IFE::RECOVERY_TAG::ARRAY_FLOAT32);
    IFE_CHECK(IFE::TypeTraits<float>::kind          == IFE::IFE_FieldKind::SCALAR);

    // BLOCK datatype binds to LayerExtent and matches the on-disk
    // LAYER_EXTENT::SIZE used by IrisCodec::Serialization.
    IFE_CHECK(IFE::TypeTraits<IFE::LayerExtentBlock>::wire_size == 12);
    IFE_CHECK(IFE::TypeTraits<IFE::LayerExtentBlock>::wire_size ==
              ::IrisCodec::Serialization::LAYER_EXTENT::SIZE);
    IFE_CHECK(IFE::TypeTraits<IFE::LayerExtentBlock>::kind ==
              IFE::IFE_FieldKind::BLOCK);
    IFE_CHECK(IFE::TypeTraits<IFE::LayerExtentBlock>::scalar_tag ==
              IFE::RECOVERY_TAG::BLOCK_LAYER_EXTENT);
    IFE_CHECK(IFE::TypeTraits<IFE::LayerExtentBlock>::array_tag ==
              IFE::RECOVERY_TAG::ARRAY_LAYER_EXTENT);
}

}  // namespace

int main() {
    test_legacy_round_trip();
    test_partition_helpers();
    test_type_traits();

    if (g_failures) {
        std::fprintf(stderr, "ife_types_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "ife_types_tests: OK\n");
    return 0;
}
