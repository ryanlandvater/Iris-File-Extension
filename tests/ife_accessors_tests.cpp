/**
 * @file ife_accessors_tests.cpp
 * @brief Round-trip tests for the codegen-emitted IFE_Accessors.hpp.
 *
 * The Phase 6b accessors are the foundation that future PRs will grow into a
 * full schema-regenerated serializer (eventually replacing the hand-written
 * IrisCodecExtension.cpp). They need to hold three invariants:
 *
 *   1. **Per-field round-trip.** Every `accessors::<RES>::write_<FIELD>`
 *      followed by `read_<FIELD>` returns the value that was written, for
 *      every field of every resource in the schema.
 *
 *   2. **Record round-trip.** `accessors::<RES>::encode(buf, record)` followed
 *      by `decode(buf)` returns a byte-equal record, including the narrow-
 *      packed types (uint24, uint40) used in the sub-block datatypes.
 *
 *   3. **Reflective parity.** A value written through the accessor is read
 *      back identically by `IFE::Reflective::Node::field<T>` — i.e. the
 *      accessor offsets agree with the codegen vtable that the reflective
 *      lens consults.
 *
 * Plus packed-LE primitive checks: load/store_uN_le<N> for every supported N,
 * sign_extend_uN at the boundary, and the per-type narrow load/store helpers.
 */
#include "IFE_Accessors.hpp"
#include "IFE_Array.hpp"
#include "IFE_Builder.hpp"
#include "IFE_DataBlock.hpp"
#include "IFE_DataTypes.hpp"
#include "IFE_Memory.hpp"
#include "IFE_Reflective.hpp"
#include "IFE_VTables.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace A = ::IFE::accessors;
namespace V = ::IFE::vtables;

namespace {

[[noreturn]] void fail(const char* what, int line) {
    std::fprintf(stderr, "ife_accessors_tests: FAIL @ %d: %s\n", line, what);
    std::exit(1);
}

#define IFE_REQUIRE(cond) \
    do { if (!(cond)) fail(#cond, __LINE__); } while (0)

// ---- Packed-LE primitives -------------------------------------------------

void test_packed_le_primitives() {
    // load_uN_le / store_uN_le for N in 1..8 — value preserved.
    std::uint8_t buf[8] = {};
    A::detail::store_uN_le<3>(buf, 0x00ABCDEFULL);
    IFE_REQUIRE(buf[0] == 0xEF);
    IFE_REQUIRE(buf[1] == 0xCD);
    IFE_REQUIRE(buf[2] == 0xAB);
    IFE_REQUIRE(A::detail::load_uN_le<3>(buf) == 0x00ABCDEFULL);

    // uint40 round-trip: full 5-byte range.
    std::uint8_t b40[5] = {};
    const std::uint64_t v40 = 0xFEDCBA9876ULL;
    A::detail::store_uint40(b40, v40);
    IFE_REQUIRE(A::detail::load_uint40(b40) == v40);

    // uint24 round-trip.
    std::uint8_t b24[3] = {};
    A::detail::store_uint24(b24, 0xFFFFFF);
    IFE_REQUIRE(A::detail::load_uint24(b24) == 0xFFFFFFu);
    A::detail::store_uint24(b24, 0u);
    IFE_REQUIRE(A::detail::load_uint24(b24) == 0u);

    // sign_extend_uN: the most-negative 24-bit and 40-bit values come back
    // as their full-width signed counterparts.
    IFE_REQUIRE(A::detail::sign_extend_uN<3>(0x800000ULL) ==
                static_cast<std::int64_t>(-(1LL << 23)));
    IFE_REQUIRE(A::detail::sign_extend_uN<3>(0x7FFFFFULL) ==
                static_cast<std::int64_t>((1LL << 23) - 1));
    IFE_REQUIRE(A::detail::sign_extend_uN<5>(0x8000000000ULL) ==
                static_cast<std::int64_t>(-(1LL << 39)));
    IFE_REQUIRE(A::detail::sign_extend_uN<5>(0x7FFFFFFFFFULL) ==
                static_cast<std::int64_t>((1LL << 39) - 1));

    // int24 / int40 round-trip including negatives.
    std::uint8_t bi24[3] = {};
    A::detail::store_int24(bi24, -1);
    IFE_REQUIRE(A::detail::load_int24(bi24) == -1);
    A::detail::store_int24(bi24, -(1 << 23));
    IFE_REQUIRE(A::detail::load_int24(bi24) == -(1 << 23));

    std::uint8_t bi40[5] = {};
    A::detail::store_int40(bi40, -1);
    IFE_REQUIRE(A::detail::load_int40(bi40) == -1);
    A::detail::store_int40(bi40, -(1LL << 39));
    IFE_REQUIRE(A::detail::load_int40(bi40) == -(1LL << 39));

    // float / double passthrough.
    std::uint8_t bf32[4] = {};
    A::detail::store_f32(bf32, 3.5f);
    IFE_REQUIRE(A::detail::load_f32(bf32) == 3.5f);
    std::uint8_t bf64[8] = {};
    A::detail::store_f64(bf64, 1234.5);
    IFE_REQUIRE(A::detail::load_f64(bf64) == 1234.5);
}

// ---- Datatype encode/decode round-trips ------------------------------------

void test_datatype_round_trips() {
    // LAYER_EXTENT (uint32, uint32, float32) — wire_size 12.
    {
        std::uint8_t buf[::IFE::datatypes::LAYER_EXTENT::wire_size] = {};
        ::IFE::datatypes::LAYER_EXTENT in{};
        in.X_TILES = 0xDEADBEEFu;
        in.Y_TILES = 0x12345678u;
        in.SCALE   = 0.25f;
        A::LAYER_EXTENT::encode(buf, in);
        const auto out = A::LAYER_EXTENT::decode(buf);
        IFE_REQUIRE(out.X_TILES == in.X_TILES);
        IFE_REQUIRE(out.Y_TILES == in.Y_TILES);
        IFE_REQUIRE(out.SCALE   == in.SCALE);
    }
    // TILE_OFFSET (uint40 OFFSET, uint24 TILE_SIZE) — narrow-packed.
    {
        std::uint8_t buf[::IFE::datatypes::TILE_OFFSET::wire_size] = {};
        ::IFE::datatypes::TILE_OFFSET in{};
        in.OFFSET    = 0xFEDCBA9876ULL;       // representative 40-bit value
        in.TILE_SIZE = 0x00ABCDEFu;           // representative 24-bit value
        A::TILE_OFFSET::encode(buf, in);
        const auto out = A::TILE_OFFSET::decode(buf);
        IFE_REQUIRE(out.OFFSET    == in.OFFSET);
        IFE_REQUIRE(out.TILE_SIZE == in.TILE_SIZE);
        // Spot-check the wire layout: low 5 bytes are OFFSET LE.
        std::uint64_t reread = 0;
        std::memcpy(&reread, buf, 5);
        IFE_REQUIRE(reread == in.OFFSET);
    }
    // ATTRIBUTE_SIZE (uint16 + uint32).
    {
        std::uint8_t buf[::IFE::datatypes::ATTRIBUTE_SIZE::wire_size] = {};
        ::IFE::datatypes::ATTRIBUTE_SIZE in{0xBEEF, 0xDEADBEEFu};
        A::ATTRIBUTE_SIZE::encode(buf, in);
        const auto out = A::ATTRIBUTE_SIZE::decode(buf);
        IFE_REQUIRE(out.KEY_SIZE   == in.KEY_SIZE);
        IFE_REQUIRE(out.VALUE_SIZE == in.VALUE_SIZE);
    }
    // IMAGE_ENTRY (mix of widths).
    {
        std::uint8_t buf[::IFE::datatypes::IMAGE_ENTRY::wire_size] = {};
        ::IFE::datatypes::IMAGE_ENTRY in{};
        in.BYTES_OFFSET = 0x0102030405060708ULL;
        in.WIDTH        = 1024;
        in.HEIGHT       = 768;
        in.ENCODING     = 7;
        in.FORMAT       = 3;
        in.ORIENTATION  = 0xC0DE;
        A::IMAGE_ENTRY::encode(buf, in);
        const auto out = A::IMAGE_ENTRY::decode(buf);
        IFE_REQUIRE(out.BYTES_OFFSET == in.BYTES_OFFSET);
        IFE_REQUIRE(out.WIDTH        == in.WIDTH);
        IFE_REQUIRE(out.HEIGHT       == in.HEIGHT);
        IFE_REQUIRE(out.ENCODING     == in.ENCODING);
        IFE_REQUIRE(out.FORMAT       == in.FORMAT);
        IFE_REQUIRE(out.ORIENTATION  == in.ORIENTATION);
    }
    // ANNOTATION_ENTRY exercises uint24 IDENTIFIER + PARENT and float fields.
    {
        std::uint8_t buf[::IFE::datatypes::ANNOTATION_ENTRY::wire_size] = {};
        ::IFE::datatypes::ANNOTATION_ENTRY in{};
        in.IDENTIFIER   = 0xABCDEFu;
        in.BYTES_OFFSET = 0x0102030405060708ULL;
        in.FORMAT       = 11;
        in.X_LOCATION   = 0.125f;
        in.Y_LOCATION   = -0.5f;
        in.X_SIZE       = 16.0f;
        in.Y_SIZE       = 32.0f;
        in.WIDTH        = 4096;
        in.HEIGHT       = 2048;
        in.PARENT       = 0x123456u;
        A::ANNOTATION_ENTRY::encode(buf, in);
        const auto out = A::ANNOTATION_ENTRY::decode(buf);
        IFE_REQUIRE(out.IDENTIFIER   == in.IDENTIFIER);
        IFE_REQUIRE(out.BYTES_OFFSET == in.BYTES_OFFSET);
        IFE_REQUIRE(out.FORMAT       == in.FORMAT);
        IFE_REQUIRE(out.X_LOCATION   == in.X_LOCATION);
        IFE_REQUIRE(out.Y_LOCATION   == in.Y_LOCATION);
        IFE_REQUIRE(out.X_SIZE       == in.X_SIZE);
        IFE_REQUIRE(out.Y_SIZE       == in.Y_SIZE);
        IFE_REQUIRE(out.WIDTH        == in.WIDTH);
        IFE_REQUIRE(out.HEIGHT       == in.HEIGHT);
        IFE_REQUIRE(out.PARENT       == in.PARENT);
    }
    // ANNOTATION_GROUP_SIZE (uint16 + uint32).
    {
        std::uint8_t buf[::IFE::datatypes::ANNOTATION_GROUP_SIZE::wire_size] = {};
        ::IFE::datatypes::ANNOTATION_GROUP_SIZE in{42, 0x80000001u};
        A::ANNOTATION_GROUP_SIZE::encode(buf, in);
        const auto out = A::ANNOTATION_GROUP_SIZE::decode(buf);
        IFE_REQUIRE(out.LABEL_SIZE     == in.LABEL_SIZE);
        IFE_REQUIRE(out.ENTRIES_NUMBER == in.ENTRIES_NUMBER);
    }
}

// ---- Resource record round-trips + reflective parity -----------------------

void test_file_header_record_and_reflective_parity() {
    // Build a real arena via Builder + claim_file_header so the universal
    // preamble carries IFE_FILE_MAGIC and the recovery tag is RESOURCE_HEADER
    // — exactly the layout a future regenerated serializer will emit.
    auto mem = ::IFE::Memory::create(64 * 1024);
    ::IFE::Builder b{mem};
    const std::size_t body_bytes =
        V::FILE_HEADER::header_size - ::IFE::DATA_BLOCK_HEADER_SIZE;
    auto h = b.claim_file_header(body_bytes);
    IFE_REQUIRE(h.valid());

    // Populate every FILE_HEADER field through the codegen accessor record.
    A::FILE_HEADER::Record rec{};
    rec.VALIDATION        = ::IFE::IFE_FILE_MAGIC;  // already written by claim_file_header
    rec.RECOVERY          = static_cast<std::uint16_t>(
                                ::IFE::RECOVERY_TAG::RESOURCE_HEADER);
    rec.EXTENSION_MAJOR   = 2;
    rec.EXTENSION_MINOR   = 1;
    rec.FILE_REVISION     = 0xDEADBEEFu;
    rec.FILE_SIZE         = 0x0102030405060708ULL;
    rec.TILE_TABLE_OFFSET = 0xCAFEF00DCAFEF00DULL;
    rec.METADATA_OFFSET   = 0xFEEDFACEFEEDFACEULL;
    A::FILE_HEADER::encode(mem.data() + h.offset, rec);
    const auto out = A::FILE_HEADER::decode(mem.data() + h.offset);
    IFE_REQUIRE(out.VALIDATION        == rec.VALIDATION);
    IFE_REQUIRE(out.RECOVERY          == rec.RECOVERY);
    IFE_REQUIRE(out.EXTENSION_MAJOR   == rec.EXTENSION_MAJOR);
    IFE_REQUIRE(out.EXTENSION_MINOR   == rec.EXTENSION_MINOR);
    IFE_REQUIRE(out.FILE_REVISION     == rec.FILE_REVISION);
    IFE_REQUIRE(out.FILE_SIZE         == rec.FILE_SIZE);
    IFE_REQUIRE(out.TILE_TABLE_OFFSET == rec.TILE_TABLE_OFFSET);
    IFE_REQUIRE(out.METADATA_OFFSET   == rec.METADATA_OFFSET);

    // Reflective parity: every value written through the accessor is read
    // back identically through `Reflective::Node::field<T>`. This is the
    // critical guarantee that accessor offsets agree with the codegen vtable.
    ::IFE::Reflective::Node node(mem.view(), h.offset);
    IFE_REQUIRE(node.recovery_tag() == ::IFE::RECOVERY_TAG::RESOURCE_HEADER);
    IFE_REQUIRE(node.validation()   == ::IFE::IFE_FILE_MAGIC);
    IFE_REQUIRE(node.field<std::uint16_t>("EXTENSION_MAJOR") == rec.EXTENSION_MAJOR);
    IFE_REQUIRE(node.field<std::uint16_t>("EXTENSION_MINOR") == rec.EXTENSION_MINOR);
    IFE_REQUIRE(node.field<std::uint32_t>("FILE_REVISION")   == rec.FILE_REVISION);
    IFE_REQUIRE(node.field<std::uint64_t>("FILE_SIZE")       == rec.FILE_SIZE);
    IFE_REQUIRE(node.field<std::uint64_t>("TILE_TABLE_OFFSET") ==
                rec.TILE_TABLE_OFFSET);
    IFE_REQUIRE(node.field<std::uint64_t>("METADATA_OFFSET") == rec.METADATA_OFFSET);

    // And read individual fields through the per-field accessors directly to
    // confirm they match what the record decode returned.
    const std::uint8_t* base = mem.data() + h.offset;
    IFE_REQUIRE(A::FILE_HEADER::read_FILE_REVISION(base) == rec.FILE_REVISION);
    IFE_REQUIRE(A::FILE_HEADER::read_FILE_SIZE(base)     == rec.FILE_SIZE);
}

void test_metadata_record_with_floats() {
    // METADATA carries float32 fields (MICRONS_PIXEL, MAGNIFICATION) — exercise
    // the f32 accessor path through a real resource encode/decode.
    auto mem = ::IFE::Memory::create(64 * 1024);
    ::IFE::Builder b{mem};
    const std::size_t body_bytes =
        V::METADATA::header_size - ::IFE::DATA_BLOCK_HEADER_SIZE;
    auto h = b.claim_block(::IFE::RECOVERY_TAG::RESOURCE_METADATA, body_bytes);
    IFE_REQUIRE(h.valid());

    A::METADATA::Record rec{};
    rec.VALIDATION         = 0x1122334455667788ULL;
    rec.RECOVERY           = static_cast<std::uint16_t>(
                                ::IFE::RECOVERY_TAG::RESOURCE_METADATA);
    rec.CODEC_MAJOR        = 1;
    rec.CODEC_MINOR        = 2;
    rec.CODEC_BUILD        = 3;
    rec.ATTRIBUTES_OFFSET  = 0x10;
    rec.IMAGES_OFFSET      = 0x20;
    rec.ICC_COLOR_OFFSET   = 0x30;
    rec.ANNOTATIONS_OFFSET = 0x40;
    rec.MICRONS_PIXEL      = 0.25f;
    rec.MAGNIFICATION      = 40.0f;
    A::METADATA::encode(mem.data() + h.offset, rec);
    const auto out = A::METADATA::decode(mem.data() + h.offset);
    IFE_REQUIRE(out.VALIDATION         == rec.VALIDATION);
    IFE_REQUIRE(out.CODEC_MAJOR        == rec.CODEC_MAJOR);
    IFE_REQUIRE(out.CODEC_MINOR        == rec.CODEC_MINOR);
    IFE_REQUIRE(out.CODEC_BUILD        == rec.CODEC_BUILD);
    IFE_REQUIRE(out.ATTRIBUTES_OFFSET  == rec.ATTRIBUTES_OFFSET);
    IFE_REQUIRE(out.IMAGES_OFFSET      == rec.IMAGES_OFFSET);
    IFE_REQUIRE(out.ICC_COLOR_OFFSET   == rec.ICC_COLOR_OFFSET);
    IFE_REQUIRE(out.ANNOTATIONS_OFFSET == rec.ANNOTATIONS_OFFSET);
    IFE_REQUIRE(out.MICRONS_PIXEL      == rec.MICRONS_PIXEL);
    IFE_REQUIRE(out.MAGNIFICATION      == rec.MAGNIFICATION);

    // Reflective parity for the float fields (exercises the f32 path).
    ::IFE::Reflective::Node node(mem.view(), h.offset);
    IFE_REQUIRE(node.field<float>("MICRONS_PIXEL") == rec.MICRONS_PIXEL);
    IFE_REQUIRE(node.field<float>("MAGNIFICATION") == rec.MAGNIFICATION);
}

void test_tile_offsets_record_with_signed_widths() {
    // TILE_OFFSETS carries int16/int32 schema fields — exercise the signed
    // accessor path through a real resource record.
    auto mem = ::IFE::Memory::create(64 * 1024);
    ::IFE::Builder b{mem};
    const std::size_t body_bytes =
        V::TILE_OFFSETS::header_size - ::IFE::DATA_BLOCK_HEADER_SIZE;
    auto h = b.claim_block(::IFE::RECOVERY_TAG::RESOURCE_TILE_OFFSETS,
                           body_bytes);
    A::TILE_OFFSETS::Record rec{};
    rec.VALIDATION   = 0xCAFEBABEDEADBEEFULL;
    rec.RECOVERY     = static_cast<std::uint16_t>(
                          ::IFE::RECOVERY_TAG::RESOURCE_TILE_OFFSETS);
    rec.ENTRY_SIZE   = static_cast<std::int16_t>(-1);
    rec.ENTRY_NUMBER = static_cast<std::int32_t>(-1);
    A::TILE_OFFSETS::encode(mem.data() + h.offset, rec);
    const auto out = A::TILE_OFFSETS::decode(mem.data() + h.offset);
    IFE_REQUIRE(out.VALIDATION   == rec.VALIDATION);
    IFE_REQUIRE(out.ENTRY_SIZE   == rec.ENTRY_SIZE);
    IFE_REQUIRE(out.ENTRY_NUMBER == rec.ENTRY_NUMBER);
}

}  // namespace

int main() {
    test_packed_le_primitives();
    test_datatype_round_trips();
    test_file_header_record_and_reflective_parity();
    test_metadata_record_with_floats();
    test_tile_offsets_record_with_signed_widths();
    std::printf("ife_accessors_tests: PASSED\n");
    return 0;
}
