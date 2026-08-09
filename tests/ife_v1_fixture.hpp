/**
 * @file ife_v1_fixture.hpp
 * @brief A slide file built by the SHIPPED encoder, for tests that read it back.
 *
 * Deliberately free of both layers' types. IFE_Runtime.hpp and
 * IrisCodecExtension.hpp are mutually exclusive by design — both define
 * IrisCodec::Abstraction, which is what lets a consumer switch between them by
 * changing one include line — so a test that wants v1 to *write* and the new
 * runtime to *read* cannot have them in one translation unit.
 *
 * The seam is this header: `ife_v1_fixture.cpp` includes the hand-written
 * layer and produces bytes; the test includes the generated runtime and
 * consumes them. Nothing but `std::vector<unsigned char>` crosses between.
 */

#ifndef ife_v1_fixture_hpp
#define ife_v1_fixture_hpp

#include <cstdint>
#include <string>
#include <vector>

namespace v1_fixture {

/// The values the fixture encodes, so a reader can assert against the inputs
/// rather than against another reader.
struct Expected {
    std::uint64_t file_size      = 0;
    std::uint32_t revision       = 0;
    std::uint32_t x_extent       = 0;
    std::uint32_t y_extent       = 0;
    float         microns        = 0.f;
    float         magnification  = 0.f;
    std::uint32_t layers         = 0;
    std::uint32_t tiles          = 0;   ///< total across all layers
    std::string   icc_profile;
    std::string   image_label;
    std::uint32_t image_width    = 0;
    std::uint32_t image_height   = 0;
    std::string   attribute_key;
    std::string   attribute_value;
    std::uint64_t tile_table_at  = 0;
    std::uint64_t metadata_at    = 0;
};

/// The values the fixture encodes. Pure data, so a test that cannot link the
/// hand-written layer still knows what to expect; the derived members
/// (file_size and the block offsets) are filled in by build_slide, which needs
/// v1's own size arithmetic to compute them.
inline Expected expectations() {
    Expected e;
    e.revision        = 0x00C0FFEEu;
    e.x_extent        = 2048;
    e.y_extent        = 1024;
    e.microns         = 0.2467f;
    e.magnification   = 40.0f;
    e.layers          = 3;
    e.tiles           = 2 * 2 + 4 * 4 + 8 * 8;
    e.icc_profile     = "ICC-PROFILE-BYTES";
    e.image_label     = "thumbnail";
    e.image_width     = 512;
    e.image_height    = 256;
    e.attribute_key   = "SCANNER";
    e.attribute_value = "TestCo";
    return e;
}

/// Build a complete, valid slide file using v1's STORE_* functions only.
std::vector<unsigned char> build_slide(Expected& __expected);

}  // namespace v1_fixture

#endif  // ife_v1_fixture_hpp
