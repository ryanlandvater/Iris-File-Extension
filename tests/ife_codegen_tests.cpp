/**
 * @file ife_codegen_tests.cpp
 * @brief Sanity tests for the schema-driven codegen.
 *
 * Codegen (`tools/ifc.py`) is the single source of truth for the IFE wire
 * format. The legacy `IrisCodec::Serialization::*` enums no longer exist
 * inside the substrate, so this file verifies the codegen output is
 * *internally consistent* — schema invariants the generated headers must
 * satisfy regardless of any external reference:
 *
 *   - Schema version is pinned at codegen time.
 *   - Every resource's `header_size` equals the universal preamble +
 *     the sum of its field sizes (== the offset of the synthetic
 *     "after the last field" boundary).
 *   - Every resource's `recovery_tag` lives in the RESOURCE partition.
 *   - The `IFE_Resources` dispatch table contains the same set of
 *     resources as the codegen `field_keys::*` namespaces (one entry
 *     per resource, all reachable via `lookup`).
 *   - FILE_HEADER leads with the universal preamble.
 *
 * Most checks are compile-time; a trivial `main` runs the table sanity
 * walk so ctest reports a passing executable.
 */
#include "IFE_DataBlock.hpp"
#include "IFE_DataTypes.hpp"
#include "IFE_FieldKeys.hpp"
#include "IFE_Resources.hpp"
#include "IFE_Types.hpp"
#include "IFE_VTables.hpp"

#include <cstdio>

namespace G  = ::IFE::vtables;
namespace GD = ::IFE::datatypes;
namespace GK = ::IFE::field_keys;
namespace GR = ::IFE::resources;

// -----------------------------------------------------------------------------
// FILE_HEADER must lead with the universal 10-byte preamble.
// -----------------------------------------------------------------------------
static_assert(G::FILE_HEADER::offset::VALIDATION == 0,
              "FILE_HEADER must lead with VALIDATION at offset 0");
static_assert(G::FILE_HEADER::size::VALIDATION   == 8,
              "FILE_HEADER VALIDATION must be uint64");
static_assert(G::FILE_HEADER::offset::RECOVERY   == 8,
              "FILE_HEADER RECOVERY must immediately follow VALIDATION");
static_assert(G::FILE_HEADER::size::RECOVERY     == 2,
              "FILE_HEADER RECOVERY must be uint16");
static_assert(G::FILE_HEADER::recovery_tag       == ::IFE::RECOVERY_TAG::RESOURCE_HEADER);

// -----------------------------------------------------------------------------
// Schema version pinning. The codegen embeds the schema version as
// constexpr globals so external translation units that have been compiled
// against an older schema fail to link against a newer one.
// -----------------------------------------------------------------------------
static_assert(::IFE::IFE_SCHEMA_VERSION_MAJOR == 1, "schema version_major drift");
static_assert(::IFE::IFE_SCHEMA_VERSION_MINOR == 0, "schema version_minor drift");

// -----------------------------------------------------------------------------
// Resource dispatch table — every entry must round-trip via `lookup`.
// -----------------------------------------------------------------------------
static_assert(GR::table_size > 0, "resource dispatch table is empty");
static_assert(GR::lookup(::IFE::RECOVERY_TAG::RESOURCE_HEADER) != nullptr);
static_assert(GR::lookup(::IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE) != nullptr);
static_assert(GR::lookup(::IFE::RECOVERY_TAG::UNDEFINED) == nullptr);

// FieldKey table sanity: FILE_HEADER is the canonical reference. Anything
// else would catch an empty-array codegen bug.
static_assert(GK::FILE_HEADER::fields.size() > 0,
              "FILE_HEADER FieldKey table must not be empty");
static_assert(GK::FILE_HEADER::fields[0].name == "VALIDATION");
static_assert(GK::FILE_HEADER::fields[0].offset == G::FILE_HEADER::offset::VALIDATION);
static_assert(GK::FILE_HEADER::fields[0].size   == G::FILE_HEADER::size::VALIDATION);
static_assert(GK::FILE_HEADER::fields[0].kind   == ::IFE::IFE_FieldKind::SCALAR);
static_assert(GK::FILE_HEADER::fields[0].scalar_tag == ::IFE::RECOVERY_TAG::SCALAR_UINT64);

// -----------------------------------------------------------------------------
// Datatype `wire_size` self-pinning: the generated PODs sum the schema
// field widths into `wire_size`. Pin a few representative datatypes here;
// adding new datatypes does not require changes to this test.
// -----------------------------------------------------------------------------
static_assert(GD::LAYER_EXTENT::wire_size == 12,
              "LAYER_EXTENT wire_size drift (uint32+uint32+float32 == 12)");
static_assert(sizeof(GD::LAYER_EXTENT) == GD::LAYER_EXTENT::wire_size,
              "LAYER_EXTENT in-memory layout diverged from its wire size");
static_assert(GD::TILE_OFFSET::wire_size == 8,
              "TILE_OFFSET wire_size drift (uint40+uint24 == 8)");

int main() {
    int failures = 0;

    // Walk the dispatch table and verify each entry is internally consistent:
    //   - lookup() round-trips,
    //   - tag is in the RESOURCE partition,
    //   - header_size includes the universal preamble,
    //   - the FieldKey array is non-empty for every resource.
    for (std::size_t i = 0; i < GR::table_size; ++i) {
        const auto& info = GR::table[i];
        if (GR::lookup(info.tag) != &info) {
            std::fprintf(stderr,
                         "FAIL: lookup(table[%zu].tag) did not round-trip\n", i);
            ++failures;
        }
        const auto raw = ::IFE::to_underlying(info.tag);
        if (::IFE::RecoveryPartition(raw) != ::IFE::RECOVERY_PARTITION_RESOURCE) {
            std::fprintf(stderr,
                         "FAIL: %.*s tag 0x%04x is not in the RESOURCE partition\n",
                         static_cast<int>(info.name.size()), info.name.data(),
                         static_cast<unsigned>(raw));
            ++failures;
        }
        if (info.header_size < ::IFE::DATA_BLOCK_HEADER_SIZE) {
            std::fprintf(stderr,
                         "FAIL: %.*s header_size=%zu < preamble (%zu)\n",
                         static_cast<int>(info.name.size()), info.name.data(),
                         info.header_size, ::IFE::DATA_BLOCK_HEADER_SIZE);
            ++failures;
        }
        if (info.field_count == 0 || info.fields == nullptr) {
            std::fprintf(stderr,
                         "FAIL: %.*s has empty FieldKey table\n",
                         static_cast<int>(info.name.size()), info.name.data());
            ++failures;
        }
        // Field offsets must be monotonic and the last-field end must equal
        // header_size. This catches silent re-orderings and drift.
        std::size_t expected = 0;
        for (std::size_t f = 0; f < info.field_count; ++f) {
            const auto& fk = info.fields[f];
            if (fk.offset != expected) {
                std::fprintf(stderr,
                             "FAIL: %.*s::%.*s offset=%zu expected=%zu\n",
                             static_cast<int>(info.name.size()), info.name.data(),
                             static_cast<int>(fk.name.size()), fk.name.data(),
                             fk.offset, expected);
                ++failures;
            }
            expected = fk.offset + fk.size;
        }
        if (expected != info.header_size) {
            std::fprintf(stderr,
                         "FAIL: %.*s end-of-fields=%zu != header_size=%zu\n",
                         static_cast<int>(info.name.size()), info.name.data(),
                         expected, info.header_size);
            ++failures;
        }
    }

    if (failures) {
        std::fprintf(stderr, "ife_codegen_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "ife_codegen_tests: OK (%zu resources)\n",
                 GR::table_size);
    return 0;
}
