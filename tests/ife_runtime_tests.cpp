/**
 * @file ife_runtime_tests.cpp
 * @brief The public API, against a file the shipped encoder wrote.
 *
 * End-to-end: the snapshot the shipped encoder wrote is read through the
 * public entry points — generated handles over IFE_Bytes, the semantic layer
 * on top — the same way Iris-Codec calls them.
 *
 * This translation unit includes IrisFileExtension.hpp and the type-free fixture
 * loader; the bytes come from the fetched corpus (tests/corpus/README.md).
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IrisFileExtension.hpp"

// The corruption test below has to reach one field of one entry to break it.
// Included for the generated offsets rather than to test the block layer,
// which ife_blocks_tests owns: a test that hand-computes a byte position
// stops testing the format and starts testing its own arithmetic.
#include "IFE_Blocks.hpp"

#include "ife_corpus_path.hpp"
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

/// Path to the fetched corpus, from argv[1].
///
/// An argument rather than a compile definition, because the definition had to
/// survive a C string literal and a Windows path does not: the compiler reads
/// the backslashes in `D:\a\Iris-File-Extension\...` as escape sequences, so
/// \a became a bell and \b a backspace and the program never held the path at
/// all. CTest passes it through untouched.
std::string g_corpus_dir;

/// Read the snapshot the shipped encoder wrote. The bytes under test were
/// produced by the implementation that has been writing real slides, not by
/// this test; they are pinned by digest in tests/corpus/manifest.json and
/// fetched into .deps/corpus/ at configure time.
std::vector<BYTE> v1_slide(v1_fixture::Expected& expected) {
    expected = v1_fixture::expectations();

    // .test_slide, not .iris: nothing should mistake a build-tree fixture
    // for a real slide, and no tool should try to open it as one.
    const std::string path = g_corpus_dir + "/v1_0_witness.test_slide";
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) { std::fprintf(stderr, "FAIL: no snapshot at %s\n", path.c_str()); ++g_failures; return {}; }
    std::fseek(in, 0, SEEK_END);
    // The file's own length, not a number this test computed: the shipped
    // encoder decided how big its blocks are, and the size on disk is the
    // only honest source.
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

    IFE_CHECK(IrisCodec::is_iris_codec_file({f.data(), f.size()}));

    const auto result = IrisCodec::validate_file_structure({f.data(), f.size()});
    IFE_CHECK(result == Iris::IRIS_SUCCESS);
    if (result != Iris::IRIS_SUCCESS) std::fprintf(stderr, "  %s\n", result.message.c_str());

    // Not an Iris file, and not a crash: the first four bytes decide.
    std::vector<BYTE> noise(64, 0x00);
    IFE_CHECK(!IrisCodec::is_iris_codec_file({noise.data(), noise.size()}));
    // Nor is a file too short to hold a header.
    IFE_CHECK(!IrisCodec::is_iris_codec_file({f.data(), 4}));
}

void test_abstraction_matches_what_was_encoded() {
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    const auto slide = IrisCodec::abstract_file_structure({f.data(), f.size()});

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
    // The flat map carries the text values and only those: a sequence has no
    // representation in a map of string to string, which is why the tree
    // exists beside it rather than instead of it.
    IFE_CHECK(slide.metadata.attributes.size() == 1);

    // ---- the attribute tree ----------------------------------------------- //
    // The abstraction's own descent, over pinned bytes. Everything above this
    // reads the wire through generated handles; this is the one assertion that
    // the runtime lifts a nested structure into something a caller can use.
    IFE_CHECK(slide.attributeTree.size() == 1 + expected.nested_attributes.size());
    if (slide.attributeTree.size() == 1 + expected.nested_attributes.size()) {
        IFE_CHECK(slide.attributeTree[0].key == expected.attribute_key);
        IFE_CHECK(slide.attributeTree[0].nested == false);
        IFE_CHECK(slide.attributeTree[0].items.empty());

        for (std::size_t i = 0; i < expected.nested_attributes.size(); ++i) {
            const auto& sequence = expected.nested_attributes[i];
            const auto& node     = slide.attributeTree[i + 1];
            IFE_CHECK(node.key == sequence.key);
            IFE_CHECK(node.nested);
            IFE_CHECK(node.value.empty());
            IFE_CHECK(node.items.size() == sequence.items.size());
            for (std::size_t item = 0; item < node.items.size(); ++item) {
                IFE_CHECK(node.items[item].size() == sequence.items[item].size());
                for (std::size_t j = 0; j < node.items[item].size(); ++j) {
                    const auto& leaf = node.items[item][j];
                    const std::string value(reinterpret_cast<const char*>(leaf.value.data()),
                                            leaf.value.size());
                    IFE_CHECK(leaf.nested == false);
                    IFE_CHECK(leaf.key == sequence.items[item][j].first);
                    IFE_CHECK(value    == sequence.items[item][j].second);
                }
            }
        }
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

// A nested value that is not a whole number of offsets, rejected on reading.
//
// The encode side cannot produce this -- a nested value's size is derived from
// the item count, so store() has no way to emit a partial offset -- which is
// exactly why the read side has to be tested against bytes rather than against
// a writer. The fixture is corrupted in memory: one VALUE_SIZE moved off a
// multiple of eight, everything else left alone.
void test_partial_nested_offset_is_rejected() {
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);
    IFE_CHECK(static_cast<bool>(IrisCodec::validate_file_structure({f.data(), f.size()})));

    // Find the root attributes' sizes array through the public map, then the
    // nested entry within it, rather than hard-coding either position.
    namespace b = ::IFE::blocks;
    const auto map = IrisCodec::generate_file_map({f.data(), f.size()});
    bool corrupted = false;
    for (const auto& [offset, entry] : map) {
        if (entry.type != IrisCodec::Abstraction::MAP_ENTRY_ATTRIBUTE_SIZES) continue;
        const b::ATTRIBUTE_SIZES sizes{f.data(), offset, f.size(), b::VERSION_WRITTEN};
        for (std::uint32_t i = 0; i < sizes.count() && !corrupted; ++i) {
            const auto e = sizes.entry(i);
            if (e.kind() != ::IFE::constants::AttributeKinds::ATTRIBUTE_NESTED) continue;
            if (e.value_size() == 0) continue;   // an empty sequence is already whole
            // One byte short of a whole offset: the file still fits, the run
            // still has room, and only the divisibility rule is broken.
            ::IFE::store<std::uint32_t>(
                f.data() + e.__offset + b::ATTRIBUTE_SIZES::ATTRIBUTE_SIZE::offset::VALUE_SIZE,
                e.value_size() - 1);
            corrupted = true;
        }
        if (corrupted) break;
    }
    IFE_CHECK(corrupted);   // the fixture must contain a nested value to corrupt

    // Validation rejects it, and says which rule was broken.
    const auto result = IrisCodec::validate_file_structure({f.data(), f.size()});
    IFE_CHECK(result != Iris::IRIS_SUCCESS);
    IFE_CHECK(std::string(result.message).find("whole number") != std::string::npos);

    // And the abstraction refuses to lift it rather than reading a partial
    // offset and inventing a structure the encoder never wrote.
    bool threw = false;
    try { (void)IrisCodec::abstract_file_structure({f.data(), f.size()}); }
    catch (const std::runtime_error&) { threw = true; }
    IFE_CHECK(threw);
}

// The root attributes structure of a loaded snapshot, at the version the file
// declares. Constructing at VERSION_WRITTEN instead would claim a version the
// file does not have.
::IFE::blocks::ATTRIBUTES root_attributes(std::vector<BYTE>& __f) {
    namespace b = ::IFE::blocks;
    const b::FILE_HEADER boot{__f.data(), 0, __f.size(), UINT32_MAX};
    const std::uint32_t declared =
        (static_cast<std::uint32_t>(boot.extension_major()) << 16) | boot.extension_minor();
    const b::FILE_HEADER root{__f.data(), 0, __f.size(), declared};
    return root.metadata_offset().attributes_offset();
}

/// Address of the first non-empty nested value slice in an attributes
/// structure, so a test can repoint where it leads. Null when there is none.
BYTE* first_nested_value(std::vector<BYTE>& __f, const ::IFE::blocks::ATTRIBUTES& __a) {
    namespace b = ::IFE::blocks;
    namespace k = ::IFE::constants;
    const auto sizes = __a.sizes_offset();
    const auto bytes = __a.bytes_offset();
    ::IFE::Size cursor = 0;
    for (std::uint32_t i = 0; i < sizes.count(); ++i) {
        const auto e = sizes.entry(i);
        cursor += e.key_size();
        if (e.kind() == k::AttributeKinds::ATTRIBUTE_NESTED && e.value_size() > 0)
            return __f.data() + bytes.__offset + b::ATTRIBUTE_BYTES::header_size + cursor;
        cursor += e.value_size();
    }
    return nullptr;
}

// A nested value that leads back to the structure carrying it.
//
// Reachable because the writer takes caller-supplied offsets: nothing stops an
// encoder naming an ancestor, and a validator that followed it would recurse
// until the stack ran out. The guard is what makes a hostile file a rejection
// rather than a crash, so it is worth an actual cycle rather than an argument.
void test_attribute_cycle_is_rejected() {
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);
    IFE_CHECK(static_cast<bool>(IrisCodec::validate_file_structure({f.data(), f.size()})));

    const auto attrs = root_attributes(f);
    BYTE* value = first_nested_value(f, attrs);
    IFE_CHECK(value != nullptr);
    if (!value) return;

    // Point the first sequence item at the structure that names it.
    ::IFE::store<std::uint64_t>(value, attrs.__offset);

    const auto result = IrisCodec::validate_file_structure({f.data(), f.size()});
    IFE_CHECK(result != Iris::IRIS_SUCCESS);
    IFE_CHECK(std::string(result.message).find("returns to a block") != std::string::npos);

    bool threw = false;
    try { (void)IrisCodec::abstract_file_structure({f.data(), f.size()}); }
    catch (const std::runtime_error&) { threw = true; }
    IFE_CHECK(threw);
}

// A nesting chain deeper than the runtime will follow.
//
// Distinct from the cycle above: every block here is different, so nothing
// repeats on the path and only the depth bound stops the descent. The chain is
// built with the generated writers and appended to a real file, because the
// bound is a property of the runtime rather than of any fixture.
// Append a chain of `levels` attributes structures to `f`, each level naming
// the one below it `fanout` times, and return the outermost block's offset.
// FILE_SIZE is rewritten so the appended blocks are inside the file.
//
// Fan-out is what separates the two uses: one offset per level is a chain and
// tests the depth bound; many offsets per level is a DAG whose every path is
// distinct, which is what a walk without memory pays for exponentially.
::IFE::Offset append_attribute_chain(std::vector<BYTE>& __f, std::size_t __levels,
                                     std::size_t __fanout) {
    namespace b = ::IFE::blocks;
    namespace k = ::IFE::constants;
    const ::IFE::Offset base = __f.size();
    __f.resize(base + __levels * (128 + __fanout * b::NESTED_OFFSET_SIZE));

    ::IFE::Offset cursor = base, child = 0;
    for (std::size_t i = 0; i < __levels; ++i) {
        std::vector<b::AttributeSizeEntry> e(1);
        if (i == 0) {
            e[0] = {.key = "k", .value = "leaf"};
        } else {
            e[0] = {.key    = "k",
                    .nested = std::vector<::IFE::Offset>(__fanout, child),
                    .KIND   = k::AttributeKinds::ATTRIBUTE_NESTED};
        }
        const b::AttributeSizesCreateInfo si{.entries = e};
        const b::AttributeBytesCreateInfo bi{.entries = e};
        const ::IFE::Offset s_at = cursor; cursor += b::size_of(si);
        const ::IFE::Offset b_at = cursor; cursor += b::size_of(bi);
        const ::IFE::Offset a_at = cursor; cursor += b::ATTRIBUTES::header_size;
        IFE_CHECK(static_cast<bool>(b::store(__f.data(), s_at, si)));
        IFE_CHECK(static_cast<bool>(b::store(__f.data(), b_at, bi)));
        IFE_CHECK(static_cast<bool>(b::store(__f.data(), a_at, b::AttributesCreateInfo{
            .FORMAT = k::MetadataFormats::METADATA_DICOM, .VERSION = 2024,
            .SIZES_OFFSET = s_at, .BYTES_OFFSET = b_at})));
        child = a_at;
    }
    __f.resize(cursor);
    ::IFE::store<std::uint64_t>(__f.data() + b::FILE_HEADER::offset::FILE_SIZE, __f.size());
    return child;
}

// A structure named many times over is validated once.
//
// Every path here is acyclic and none exceeds the depth bound, so neither
// guard fires -- what would otherwise make this file unreadable is arithmetic:
// twelve levels of forty-way sharing is 40^12 distinct paths through five
// kilobytes of disk. The walk remembers the blocks it has finished with, so
// the cost is the number of blocks and not the number of paths.
//
// If that memory is ever removed this test does not fail, it hangs; the target
// carries a ctest TIMEOUT so the hang is reported rather than waited on.
void test_shared_nested_structures_are_validated_once() {
    namespace b = ::IFE::blocks;
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    const ::IFE::Offset head = append_attribute_chain(f, b::MAX_BLOCK_DEPTH - 4, 40);
    BYTE* value = first_nested_value(f, root_attributes(f));
    IFE_CHECK(value != nullptr);
    if (!value) return;
    ::IFE::store<std::uint64_t>(value, head);

    // Accepted, not merely survived: the file is well formed, and a reader
    // that rejected sharing would be refusing something the format allows.
    const auto result = IrisCodec::validate_file_structure({f.data(), f.size()});
    IFE_CHECK(result == Iris::IRIS_SUCCESS);
    if (result != Iris::IRIS_SUCCESS) std::fprintf(stderr, "  %s\n", result.message.c_str());

    // The map walks the same edges on files with no validated graph behind
    // them, so it carries the same memory.
    const auto map = IrisCodec::generate_file_map({f.data(), f.size()});
    IFE_CHECK(map.size() > 0);
}

void test_attribute_nesting_depth_is_bounded() {
    namespace b = ::IFE::blocks;
    namespace k = ::IFE::constants;
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    // Longer than the runtime's own bound, which is derived from the block
    // graph's limit -- so this cannot drift from the constant it tests.
    const std::size_t chain = b::MAX_BLOCK_DEPTH;
    const ::IFE::Offset base = f.size();
    f.resize(base + chain * 128);

    ::IFE::Offset cursor = base, child = 0;
    for (std::size_t i = 0; i < chain; ++i) {
        std::vector<b::AttributeSizeEntry> e(1);
        if (i == 0) e[0] = {.key = "k", .value = "leaf"};
        else        e[0] = {.key = "k", .nested = {child},
                            .KIND = k::AttributeKinds::ATTRIBUTE_NESTED};
        const b::AttributeSizesCreateInfo si{.entries = e};
        const b::AttributeBytesCreateInfo bi{.entries = e};
        const ::IFE::Offset s_at = cursor; cursor += b::size_of(si);
        const ::IFE::Offset b_at = cursor; cursor += b::size_of(bi);
        const ::IFE::Offset a_at = cursor; cursor += b::ATTRIBUTES::header_size;
        IFE_CHECK(static_cast<bool>(b::store(f.data(), s_at, si)));
        IFE_CHECK(static_cast<bool>(b::store(f.data(), b_at, bi)));
        IFE_CHECK(static_cast<bool>(b::store(f.data(), a_at, b::AttributesCreateInfo{
            .FORMAT = k::MetadataFormats::METADATA_DICOM, .VERSION = 2024,
            .SIZES_OFFSET = s_at, .BYTES_OFFSET = b_at})));
        child = a_at;
    }
    f.resize(cursor);
    ::IFE::store<std::uint64_t>(f.data() + b::FILE_HEADER::offset::FILE_SIZE, f.size());

    // Hang the chain off the root, replacing the fixture's own first item.
    const auto attrs = root_attributes(f);
    BYTE* value = first_nested_value(f, attrs);
    IFE_CHECK(value != nullptr);
    if (!value) return;
    ::IFE::store<std::uint64_t>(value, child);

    const auto result = IrisCodec::validate_file_structure({f.data(), f.size()});
    IFE_CHECK(result != Iris::IRIS_SUCCESS);
    // Named specifically, on both axes. Nothing here repeats on the path, so
    // reporting a cycle would be wrong; and the attribute bound must be what
    // stopped the descent, not VisitPath running out of room behind it. The
    // two are told apart by the limit each reports -- the attribute bound is
    // below MAX_BLOCK_DEPTH by construction, so a message naming
    // MAX_BLOCK_DEPTH means the wrong guard fired.
    const std::string message(result.message);
    IFE_CHECK(message.find("nested") != std::string::npos);
    IFE_CHECK(message.find("returns to a block") == std::string::npos);
    IFE_CHECK(message.find("past the limit of " + std::to_string(b::MAX_BLOCK_DEPTH))
              == std::string::npos);

    bool threw = false;
    try { (void)IrisCodec::abstract_file_structure({f.data(), f.size()}); }
    catch (const std::runtime_error&) { threw = true; }
    IFE_CHECK(threw);
}

void test_file_map_finds_every_block() {
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    const auto map = IrisCodec::generate_file_map({f.data(), f.size()});
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
    // bytes, ICC, images, image bytes, the annotations array, one
    // ANNOTATION_BYTES per annotation, and three blocks per nested sequence
    // item -- its own attributes header, sizes array and byte run.
    //
    // The nested blocks are the reason the map descends attribute values at
    // all: the map exists to answer "what lies after the byte I am about to
    // overwrite", and a live block it never mentions is one it will let a
    // caller land on.
    int nested_blocks = 0;
    for (const auto& sequence : expected.nested_attributes)
        nested_blocks += 3 * static_cast<int>(sequence.items.size());
    // 14, not 12: the witness carries ANNOTATION_GROUP_SIZES and
    // ANNOTATION_GROUP_BYTES, which the 1.0 fixture gained when it was made
    // comprehensive.
    IFE_CHECK(blocks == 14 + nested_blocks + static_cast<int>(expected.annotations.size()));
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

    const auto recovered = IrisCodec::recover_file_structure({f.data(), f.size()});

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
    // ten, plus the annotations array and one ANNOTATION_BYTES apiece, plus
    // three per nested sequence item. A false positive needs eight bytes equal
    // to their own offset followed by a u16 in the 0x55 tag set -- the reason
    // that prefix is worth keeping.
    //
    // The nested blocks are the point of counting them here. This scan never
    // reads an attribute value -- the offset graph is deliberately destroyed
    // below -- so finding them proves a nested structure is an ordinary block
    // that carries its own VALIDATION word and recovery tag, and is
    // recoverable without the parent that names it.
    int nested_blocks = 0;
    for (const auto& sequence : expected.nested_attributes)
        nested_blocks += 3 * static_cast<int>(sequence.items.size());
    // 13, not 11: as above, the two group arrays are tagged blocks and a
    // scan finds them without the ANNOTATIONS block that names them.
    IFE_CHECK(recovered.size() ==
              13 + nested_blocks + expected.annotations.size());

    // What a scan cannot do is tell the root attributes structure from an
    // item: they are structurally identical, and only the reference from a
    // parent's byte run distinguishes them. Recorded here because a recovery
    // tool has to resolve that by reference, not by position.
    int attribute_headers = 0;
    for (const auto& [offset, entry] : recovered)
        if (entry.type == MAP_ENTRY_ATTRIBUTES) ++attribute_headers;
    IFE_CHECK(attribute_headers == 1 + nested_blocks / 3);

    // And the graph walk really is defeated, so the comparison is meaningful.
    bool threw = false;
    try { (void)IrisCodec::generate_file_map({f.data(), f.size()}); }
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
    namespace b  = ::IFE::blocks;

    struct Tile { std::uint32_t index; std::uint32_t size; };
    const Tile tiles[] = {{7, 300}, {2, 145}, {19, 64}};

    // A run of framed streams, nothing else -- no header, no arrays. Recovery
    // has to work from the frames alone.
    std::vector<Iris::BYTE> f(64, 0xA5);   // leading junk, so nothing sits at 0
    std::vector<IFE::Offset> stream_at;
    for (const auto& t : tiles) {
        f.resize(f.size() + b::TILE_PIXEL_DATA::header_size);
        const IFE::Offset at = f.size();
        stream_at.push_back(at);
        ::IFE::store_u40(f.data() + at + b::TILE_PIXEL_DATA::offset::VALIDATION,
                         at + b::TILE_PIXEL_DATA::offset::VALIDATION);
        ::IFE::store<std::uint32_t>(f.data() + at + b::TILE_PIXEL_DATA::offset::TILE_INDEX, t.index);
        ::IFE::store<std::uint16_t>(f.data() + at + b::TILE_PIXEL_DATA::offset::Z_PLANES, 1);
        f.resize(f.size() + t.size, 0x5A);
    }

    const auto recovered = IrisCodec::recover_file_structure({f.data(), f.size()});

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
    IFE_CHECK(IrisCodec::recover_file_structure({poisoned.data(), poisoned.size()}).size() == 6);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 2;
    }
    // Bazel cannot pass a directory; BUILD.bazel passes the runfiles path of
    // one corpus file and its parent is the directory CTest passes directly.
    g_corpus_dir = ife_corpus_dir(argv[1]);

    // Needs no slide of its own.
    test_recovery_finds_tile_frames_and_rebuilds_entries();

    // The rest read the fetched snapshot. If the corpus fetch did not run
    // there is nothing to read, and going on would turn a clear diagnostic
    // into an uncaught exception from the first handle built over an empty
    // buffer -- which is how this failure presented on Windows before argv
    // carried the path: a SEGFAULT report, with the real cause four lines
    // further up.
    v1_fixture::Expected probe;
    if (v1_slide(probe).empty()) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }

    test_validate_accepts_a_v1_file();
    test_abstraction_matches_what_was_encoded();
    test_partial_nested_offset_is_rejected();
    test_attribute_cycle_is_rejected();
    test_attribute_nesting_depth_is_bounded();
    test_shared_nested_structures_are_validated_once();
    test_file_map_finds_every_block();
    test_recovery_finds_blocks_without_the_offset_graph();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_runtime_tests: all checks passed\n");
    return 0;
}
