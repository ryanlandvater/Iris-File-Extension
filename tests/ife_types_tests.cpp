/**
 * @file ife_types_tests.cpp
 * @brief Sanity tests for IFE_Types.hpp.
 *
 * The bulk of the type-system invariants are pinned at compile time via
 * `static_assert` inside `IFE_Types.hpp` itself; getting this translation
 * unit to compile *is* most of the test. The runtime checks below cover
 * the partition helpers and `TypeTraits` accessors that the rest of the
 * substrate consumes.
 *
 * Self-contained (no external test framework). Non-zero exit on failure.
 */
#include "IFE_Types.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do {                                            \
    if (!(cond)) {                                                      \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n",                      \
                     #cond, __FILE__, __LINE__);                        \
        ++g_failures;                                                   \
    }                                                                   \
} while (0)

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

    // BLOCK datatype binds to LayerExtent. The wire size is fixed by the
    // schema (uint32 + uint32 + float32 = 12) and equals sizeof(struct)
    // because all three fields are naturally aligned.
    IFE_CHECK(IFE::TypeTraits<IFE::LayerExtentBlock>::wire_size == 12);
    IFE_CHECK(IFE::TypeTraits<IFE::LayerExtentBlock>::wire_size ==
              sizeof(IFE::LayerExtentBlock));
    IFE_CHECK(IFE::TypeTraits<IFE::LayerExtentBlock>::kind ==
              IFE::IFE_FieldKind::BLOCK);
    IFE_CHECK(IFE::TypeTraits<IFE::LayerExtentBlock>::scalar_tag ==
              IFE::RECOVERY_TAG::BLOCK_LAYER_EXTENT);
    IFE_CHECK(IFE::TypeTraits<IFE::LayerExtentBlock>::array_tag ==
              IFE::RECOVERY_TAG::ARRAY_LAYER_EXTENT);
}

}  // namespace

int main() {
    test_partition_helpers();
    test_type_traits();

    if (g_failures) {
        std::fprintf(stderr, "ife_types_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "ife_types_tests: OK\n");
    return 0;
}
