/**
 * @file ife_runtime_tests.cpp
 * @brief The public API, against a file the shipped encoder wrote.
 *
 * The end-to-end the migration exists to make true: v1 encodes, and the whole
 * new stack — generated handles over IFE_Bytes, the semantic layer on top —
 * reads it through the same four entry points Iris-Codec calls today.
 *
 * This translation unit includes IFE_Runtime.hpp and never IrisCodecExtension.hpp;
 * the encoder lives in ife_v1_fixture.cpp. That split is not incidental — the
 * two headers are mutually exclusive so that a consumer can switch layers by
 * changing one include line — the cutover.
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IFE_Runtime.hpp"

#include "ife_v1_fixture.hpp"

#include <cstdlib>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

using ::Iris::BYTE;

/// Path to the shipped encoder, from argv[1].
///
/// An argument rather than a compile definition, because the definition had to
/// survive a C string literal and a Windows path does not: the compiler reads
/// the backslashes in `D:\a\Iris-File-Extension\...` as escape sequences, so
/// \a became a bell and \b a backspace and the program never held the path at
/// all. CTest passes it through untouched.
std::string g_writer;

/// Run the shipped encoder (a separate binary -- see ife_v1_slide_writer.cpp)
/// and read back what it wrote. The bytes under test are produced by the
/// implementation that has been writing real slides, not by this test.
std::vector<BYTE> v1_slide(v1_fixture::Expected& expected) {
    expected = v1_fixture::expectations();

    // .test_slide, not .iris: nothing should mistake a build-tree fixture
    // for a real slide, and no tool should try to open it as one.
    const std::string path = g_writer + ".test_slide";
    std::string cmd = "\"" + g_writer + "\" \"" + path + "\"";
#ifdef _WIN32
    // cmd.exe strips the first and last quote of a command that begins with
    // one, which leaves the path split at its first space. Wrapping the whole
    // command in one more pair is the documented way round it.
    cmd = "\"" + cmd + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "FAIL: the v1 slide writer did not run: %s\n", cmd.c_str());
        ++g_failures;
        return {};
    }

    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) { std::fprintf(stderr, "FAIL: no slide at %s\n", path.c_str()); ++g_failures; return {}; }
    std::fseek(in, 0, SEEK_END);
    // The file's own length, not a number this test computed: v1 decided how
    // big its blocks are, and the size on disk is the only honest source.
    const auto size = static_cast<std::size_t>(std::ftell(in));
    std::fseek(in, 0, SEEK_SET);
    std::vector<BYTE> bytes(size);
    const auto read = std::fread(bytes.data(), 1, size, in);
    std::fclose(in);
    if (read != size) {
        std::fprintf(stderr, "FAIL: read %zu of %zu bytes\n", read, size);
        ++g_failures;
    }
    expected.file_size = size;
    return bytes;
}

void test_validate_accepts_a_v1_file() {
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    IFE_CHECK(IrisCodec::is_Iris_Codec_file(f.data(), f.size()));

    const auto result = IrisCodec::validate_file_structure(f.data(), f.size());
    IFE_CHECK(result == Iris::IRIS_SUCCESS);
    if (result != Iris::IRIS_SUCCESS) std::fprintf(stderr, "  %s\n", result.message.c_str());

    // Not an Iris file, and not a crash: the first four bytes decide.
    std::vector<BYTE> noise(64, 0x00);
    IFE_CHECK(!IrisCodec::is_Iris_Codec_file(noise.data(), noise.size()));
    // Nor is a file too short to hold a header.
    IFE_CHECK(!IrisCodec::is_Iris_Codec_file(f.data(), 4));
}

void test_abstraction_matches_what_was_encoded() {
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    const auto slide = IrisCodec::abstract_file_structure(f.data(), f.size());

    // ---- header ---------------------------------------------------------- //
    IFE_CHECK(slide.header.fileSize == expected.file_size);
    IFE_CHECK(slide.header.revision == expected.revision);
    IFE_CHECK((slide.header.extVersion >> 16) == 1);
    IFE_CHECK((slide.header.extVersion & 0xFFFF) == 0);

    // ---- tile table ------------------------------------------------------ //
    IFE_CHECK(slide.tileTable.encoding == IrisCodec::TILE_ENCODING_JPEG);
    IFE_CHECK(slide.tileTable.format == Iris::FORMAT_R8G8B8A8);
    IFE_CHECK(slide.tileTable.extent.width == expected.x_extent);
    IFE_CHECK(slide.tileTable.extent.height == expected.y_extent);
    IFE_CHECK(slide.tileTable.extent.layers.size() == expected.layers);

    // Downsample is derived by the runtime, not stored: the most magnified
    // layer is 1.0 and each lower layer is the reciprocal of its scale ratio.
    if (slide.tileTable.extent.layers.size() == 3) {
        IFE_CHECK(slide.tileTable.extent.layers[0].xTiles == 2);
        IFE_CHECK(slide.tileTable.extent.layers[2].xTiles == 8);
        IFE_CHECK(slide.tileTable.extent.layers[2].downsample == 1.0f);
        IFE_CHECK(std::abs(slide.tileTable.extent.layers[0].downsample - 4.0f) < 1e-6f);
        IFE_CHECK(std::abs(slide.tileTable.extent.layers[1].downsample - 2.0f) < 1e-6f);
    }

    // The flat tile-offset array split back across layers -- a semantic step,
    // not a layout one, and the place a miscount would surface.
    IFE_CHECK(slide.tileTable.layers.size() == expected.layers);
    std::uint32_t counted = 0;
    for (std::size_t li = 0; li < slide.tileTable.layers.size(); ++li) {
        const auto& extent = slide.tileTable.extent.layers[li];
        IFE_CHECK(slide.tileTable.layers[li].size() ==
                  static_cast<std::size_t>(extent.xTiles) * extent.yTiles);
        for (const auto& tile : slide.tileTable.layers[li]) {
            IFE_CHECK(tile.size == 16);
            IFE_CHECK(tile.offset + tile.size <= expected.file_size);
            ++counted;
        }
    }
    IFE_CHECK(counted == expected.tiles);

    // ---- metadata -------------------------------------------------------- //
    IFE_CHECK(slide.metadata.codec.major == 1);
    IFE_CHECK(slide.metadata.codec.minor == 2);
    IFE_CHECK(slide.metadata.codec.build == 3);
    IFE_CHECK(slide.metadata.micronsPerPixel == expected.microns);
    IFE_CHECK(slide.metadata.magnification == expected.magnification);
    IFE_CHECK(slide.metadata.ICC_profile == expected.icc_profile);

    // Attributes: a key and a value sliced out of one byte run by a parallel
    // size array. There is no string type in IFE, so this is where the absence
    // of one becomes visible.
    IFE_CHECK(slide.metadata.attributes.size() == 1);
    const auto attribute = slide.metadata.attributes.find(expected.attribute_key);
    IFE_CHECK(attribute != slide.metadata.attributes.end());
    if (attribute != slide.metadata.attributes.end()) {
        const std::string value(reinterpret_cast<const char*>(attribute->second.data()),
                                attribute->second.size());
        IFE_CHECK(value == expected.attribute_value);
    }

    // Associated images, keyed by the label sliced from the image block.
    IFE_CHECK(slide.images.size() == 1);
    IFE_CHECK(slide.metadata.associatedImages.count(expected.image_label) == 1);
    const auto image = slide.images.find(expected.image_label);
    IFE_CHECK(image != slide.images.end());
    if (image != slide.images.end()) {
        IFE_CHECK(image->second.info.width == expected.image_width);
        IFE_CHECK(image->second.info.height == expected.image_height);
        IFE_CHECK(image->second.info.encoding == IrisCodec::IMAGE_ENCODING_JPEG);
        IFE_CHECK(image->second.byteSize == 96);
        // The stream begins after the label, and the label is not part of it.
        IFE_CHECK(image->second.offset + image->second.byteSize <= expected.file_size);
        IFE_CHECK(f[image->second.offset] == 0xAB);
    }
}

void test_file_map_finds_every_block() {
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    const auto map = IrisCodec::generate_file_map(f.data(), f.size());
    IFE_CHECK(map.file_size == expected.file_size);

    // Ordered by offset, which is the property the whole API exists for:
    // "what lies after the byte I am about to overwrite".
    IFE_CHECK(map.count(0) == 1);
    IFE_CHECK(map.at(0).type == IrisCodec::Abstraction::MAP_ENTRY_FILE_HEADER);

    auto has = [&map](IrisCodec::Abstraction::MapEntryType type) {
        for (const auto& [offset, entry] : map) if (entry.type == type) return true;
        return false;
    };
    using namespace IrisCodec::Abstraction;
    for (auto type : {MAP_ENTRY_TILE_TABLE, MAP_ENTRY_METADATA, MAP_ENTRY_LAYER_EXTENTS,
                      MAP_ENTRY_TILE_OFFSETS, MAP_ENTRY_ATTRIBUTES, MAP_ENTRY_ATTRIBUTE_SIZES,
                      MAP_ENTRY_ATTRIBUTES_BYTES, MAP_ENTRY_ICC_PROFILE,
                      MAP_ENTRY_ASSOCIATED_IMAGES, MAP_ENTRY_ASSOCIATED_IMAGE_BYTES,
                      MAP_ENTRY_ANNOTATIONS, MAP_ENTRY_ANNOTATION_BYTES})
        IFE_CHECK(has(type));

    int blocks = 0, tile_data = 0;
    for (const auto& [offset, entry] : map) {
        IFE_CHECK(offset + entry.size <= expected.file_size);
        if (entry.type == IrisCodec::Abstraction::MAP_ENTRY_TILE_DATA) ++tile_data;
        else ++blocks;
    }
    // Header, tile table, extents, offsets, metadata, attributes, sizes,
    // bytes, ICC, images, image bytes, the annotations array, and one
    // ANNOTATION_BYTES per annotation.
    IFE_CHECK(blocks == 12 + static_cast<int>(expected.annotations.size()));
    IFE_CHECK(tile_data == static_cast<int>(expected.tiles));

    // upper_bound is the documented use: everything after a write point.
    const auto after = map.upper_bound(0);
    IFE_CHECK(after != map.end());
    IFE_CHECK(after->first > 0);
}

void test_recovery_finds_blocks_without_the_offset_graph() {
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    // Destroy the root's pointers. generate_file_map walks the graph and
    // cannot get past this; the recovery scan does not use the graph at all.
    std::memset(f.data() + 22, 0xFF, 16);   // TILE_TABLE_OFFSET + METADATA_OFFSET

    const auto recovered = IrisCodec::recover_file_structure(f.data(), f.size());

    // Every block except the root, which has no VALIDATION field to find:
    // it lives at byte 0, where that field could only ever store zero.
    auto found = [&recovered](IrisCodec::Abstraction::MapEntryType type) {
        for (const auto& [offset, entry] : recovered) if (entry.type == type) return true;
        return false;
    };
    using namespace IrisCodec::Abstraction;
    IFE_CHECK(found(MAP_ENTRY_TILE_TABLE));
    IFE_CHECK(found(MAP_ENTRY_METADATA));
    IFE_CHECK(found(MAP_ENTRY_LAYER_EXTENTS));
    IFE_CHECK(found(MAP_ENTRY_ICC_PROFILE));
    IFE_CHECK(recovered.count(0) == 0);   // the root has no VALIDATION to find

    IFE_CHECK(found(MAP_ENTRY_ANNOTATIONS));
    IFE_CHECK(found(MAP_ENTRY_ANNOTATION_BYTES));

    // Every self-validating block written is found, and nothing is invented:
    // ten, plus the annotations array and one ANNOTATION_BYTES apiece. A false
    // positive needs eight bytes equal to their own offset followed by a u16
    // in the 0x55 tag set -- the reason that prefix is worth keeping.
    IFE_CHECK(recovered.size() == 11 + expected.annotations.size());

    // And the graph walk really is defeated, so the comparison is meaningful.
    bool threw = false;
    try { (void)IrisCodec::generate_file_map(f.data(), f.size()); }
    catch (const std::runtime_error&) { threw = true; }
    IFE_CHECK(threw);
}


/// Tile frames are found by the same scan, and carry enough to rebuild an entry.
///
/// This is the case the frame exists for: the tile offsets array is gone, so
/// nothing says where any tile is or which tile it is. The frames are all that
/// is left, and a frame is found without a recovery tag -- what marks it is a
/// forty-bit value equal to its own position, which nothing else in the format
/// writes.
///
/// The streams here are deliberately out of index order. Ordering is explicitly
/// free (spec 2.4.3), so a scan that inferred an index from file position would
/// pass a tidy fixture and be wrong on a real one written in parallel.
void test_recovery_finds_tile_frames_and_rebuilds_entries() {
    using namespace IrisCodec::Abstraction;
    namespace vt = ::IFE::vtables;
    namespace b  = ::IFE::blocks;

    struct Tile { std::uint32_t index; std::uint32_t size; };
    const Tile tiles[] = {{7, 300}, {2, 145}, {19, 64}};

    // A run of framed streams, nothing else -- no header, no arrays. Recovery
    // has to work from the frames alone.
    std::vector<Iris::BYTE> f(64, 0xA5);   // leading junk, so nothing sits at 0
    std::vector<IFE::Offset> stream_at;
    for (const auto& t : tiles) {
        f.resize(f.size() + vt::TILE_PIXEL_DATA::header_size);
        const IFE::Offset at = f.size();
        stream_at.push_back(at);
        ::IFE::store_u40(f.data() + at + vt::TILE_FRAME::offset::VALIDATION,
                         at + vt::TILE_FRAME::offset::VALIDATION);
        ::IFE::store<std::uint32_t>(f.data() + at + vt::TILE_PIXEL_DATA::offset::TILE_INDEX, t.index);
        ::IFE::store<std::uint16_t>(f.data() + at + vt::TILE_PIXEL_DATA::offset::Z_PLANES, 1);
        f.resize(f.size() + t.size, 0x5A);
    }

    const auto recovered = IrisCodec::recover_file_structure(f.data(), f.size());

    int frames = 0, data = 0;
    for (const auto& [offset, entry] : recovered) {
        if (entry.type == MAP_ENTRY_TILE_FRAME) ++frames;
        if (entry.type == MAP_ENTRY_TILE_DATA)  ++data;
    }
    IFE_CHECK(frames == 3);
    IFE_CHECK(data == 3);

    // What recovery is actually for: turn each frame back into the tile
    // offsets entry that was lost. Index, offset and size all come from the
    // frame, so the rebuilt entry is complete rather than a location alone.
    for (std::size_t i = 0; i < std::size(tiles); ++i) {
        const b::TILE_PIXEL_DATA frame{f.data(), stream_at[i], f.size(), b::VERSION_WRITTEN};
        IFE_CHECK(static_cast<bool>(frame.validate()));
        IFE_CHECK(frame.tile_index() == tiles[i].index);

        // The recovered map agrees with the frame about where the payload is.
        // Its extent is left at zero: the frame carries no length, and how far
        // a compressed stream runs is the codec's question, not this layer's.
        const auto it = recovered.find(stream_at[i]);
        IFE_CHECK(it != recovered.end());
        if (it != recovered.end()) {
            IFE_CHECK(it->second.type == MAP_ENTRY_TILE_DATA);
            IFE_CHECK(it->second.size == 0);
        }
    }

    // Junk does not answer the self-reference test, so the leading region and
    // the payload bytes contribute nothing.
    IFE_CHECK(recovered.size() == 6);

    // A frame must fit behind the stream it precedes. A self-referencing u40
    // too near the start of file cannot be one, and is rejected on those
    // grounds -- plant exactly that and confirm nothing is invented. A guard
    // never exercised is a guard that rots.
    //
    // That bound is the only filter left now that the frame carries no length
    // to sanity-check. A u40 equal to its own position is a 2^-40 event, which
    // over a 2 GB file is an expected 0.002 false frames; the caller sees one
    // extra entry whose tile index is nonsense, and nothing worse.
    auto poisoned = f;
    constexpr IFE::Offset FAKE = 2;   // anchor would be 7, short of the 11 a frame needs
    ::IFE::store_u40(poisoned.data() + FAKE, FAKE);
    IFE_CHECK(::IFE::load_u40(poisoned.data() + FAKE) == FAKE);   // the bait is set
    IFE_CHECK(IrisCodec::recover_file_structure(poisoned.data(), poisoned.size()).size() == 6);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path to ife_v1_slide_writer>\n", argv[0]);
        return 2;
    }
    g_writer = argv[1];

    // Needs no slide of its own.
    test_recovery_finds_tile_frames_and_rebuilds_entries();

    // The rest read what the v1 writer produced. If it did not run there is
    // nothing to read, and going on would turn a clear diagnostic into an
    // uncaught exception from the first handle built over an empty buffer --
    // which is how this failure presented on Windows before argv carried the
    // path: a SEGFAULT report, with the real cause four lines further up.
    v1_fixture::Expected probe;
    if (v1_slide(probe).empty()) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }

    test_validate_accepts_a_v1_file();
    test_abstraction_matches_what_was_encoded();
    test_file_map_finds_every_block();
    test_recovery_finds_blocks_without_the_offset_graph();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_runtime_tests: all checks passed\n");
    return 0;
}
