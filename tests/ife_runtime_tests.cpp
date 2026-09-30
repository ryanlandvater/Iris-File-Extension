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
#include "IFE_Recovery.hpp"   // the file-map surface (generate_file_map, MapEntryType)

// The corruption test below has to reach one field of one entry to break it.
// Included for the generated offsets rather than to test the block layer,
// which ife_blocks_tests owns: a test that hand-computes a byte position
// stops testing the format and starts testing its own arithmetic.
#include "IFE_Blocks.hpp"
#include "IFE_Primitives.hpp"   // versioned_root, and the primitive header offsets

#include "ife_corpus_path.hpp"
#include "ife_v1_fixture.hpp"

#include <cstdlib>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
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

    IFE_CHECK(Iris::File::is_iris_codec_file({f.data(), f.size()}));

    const auto result = Iris::File::validate_file_structure({f.data(), f.size()});
    IFE_CHECK(result == Iris::IRIS_SUCCESS);
    if (result != Iris::IRIS_SUCCESS) std::fprintf(stderr, "  %s\n", result.message.c_str());

    // Not an Iris file, and not a crash: the first four bytes decide.
    std::vector<BYTE> noise(64, 0x00);
    IFE_CHECK(!Iris::File::is_iris_codec_file({noise.data(), noise.size()}));
    // Nor is a file too short to hold a header.
    IFE_CHECK(!Iris::File::is_iris_codec_file({f.data(), 4}));
}

void test_abstraction_matches_what_was_encoded() {
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    const auto slide = Iris::File::abstract_file_structure({f.data(), f.size()});

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
    IFE_CHECK(static_cast<bool>(Iris::File::validate_file_structure({f.data(), f.size()})));

    // Find the root attributes' sizes array through the public map, then the
    // nested entry within it, rather than hard-coding either position.
    namespace b = ::Iris::File::blocks;
    const auto map = Iris::File::generate_file_map({f.data(), f.size()});
    bool corrupted = false;
    for (const auto& [offset, entry] : map) {
        if (entry.type != Iris::File::Abstraction::MAP_ENTRY_ATTRIBUTE_SIZES) continue;
        const b::ATTRIBUTE_SIZES sizes{f.data(), offset, f.size(), b::VERSION_WRITTEN};
        for (std::uint32_t i = 0; i < sizes.count() && !corrupted; ++i) {
            const auto e = sizes.entry(i);
            if (e.kind() != ::Iris::File::constants::AttributeKinds::ATTRIBUTE_NESTED) continue;
            if (e.value_size() == 0) continue;   // an empty sequence is already whole
            // One byte short of a whole offset: the file still fits, the run
            // still has room, and only the divisibility rule is broken.
            ::Iris::File::store<std::uint32_t>(
                f.data() + e.__offset + b::ATTRIBUTE_SIZES::ATTRIBUTE_SIZE::offset::VALUE_SIZE,
                e.value_size() - 1);
            corrupted = true;
        }
        if (corrupted) break;
    }
    IFE_CHECK(corrupted);   // the fixture must contain a nested value to corrupt

    // Validation rejects it, and says which rule was broken.
    const auto result = Iris::File::validate_file_structure({f.data(), f.size()});
    IFE_CHECK(result != Iris::IRIS_SUCCESS);
    IFE_CHECK(std::string(result.message).find("whole number") != std::string::npos);

    // And the abstraction refuses to lift it rather than reading a partial
    // offset and inventing a structure the encoder never wrote.
    bool threw = false;
    try { (void)Iris::File::abstract_file_structure({f.data(), f.size()}); }
    catch (const std::runtime_error&) { threw = true; }
    IFE_CHECK(threw);
}

// The root attributes structure of a loaded snapshot, at the version the file
// declares. Constructing at VERSION_WRITTEN instead would claim a version the
// file does not have.
::Iris::File::blocks::ATTRIBUTES root_attributes(std::vector<BYTE>& __f) {
    namespace b = ::Iris::File::blocks;
    const b::FILE_HEADER boot{__f.data(), 0, __f.size(), UINT32_MAX};
    const std::uint32_t declared =
        (static_cast<std::uint32_t>(boot.extension_major()) << 16) | boot.extension_minor();
    const b::FILE_HEADER root{__f.data(), 0, __f.size(), declared};
    return root.metadata_offset().attributes_offset();
}

/// Address of the first non-empty nested value slice in an attributes
/// structure, so a test can repoint where it leads. Null when there is none.
BYTE* first_nested_value(std::vector<BYTE>& __f, const ::Iris::File::blocks::ATTRIBUTES& __a) {
    namespace b = ::Iris::File::blocks;
    namespace k = ::Iris::File::constants;
    const auto sizes = __a.sizes_offset();
    const auto bytes = __a.bytes_offset();
    ::Iris::File::Size cursor = 0;
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
    IFE_CHECK(static_cast<bool>(Iris::File::validate_file_structure({f.data(), f.size()})));

    const auto attrs = root_attributes(f);
    BYTE* value = first_nested_value(f, attrs);
    IFE_CHECK(value != nullptr);
    if (!value) return;

    // Point the first sequence item at the structure that names it.
    ::Iris::File::store<std::uint64_t>(value, attrs.__offset);

    const auto result = Iris::File::validate_file_structure({f.data(), f.size()});
    IFE_CHECK(result != Iris::IRIS_SUCCESS);
    IFE_CHECK(std::string(result.message).find("returns to a block") != std::string::npos);

    bool threw = false;
    try { (void)Iris::File::abstract_file_structure({f.data(), f.size()}); }
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
::Iris::File::Offset append_attribute_chain(std::vector<BYTE>& __f, std::size_t __levels,
                                     std::size_t __fanout) {
    namespace b = ::Iris::File::blocks;
    namespace k = ::Iris::File::constants;
    const ::Iris::File::Offset base = __f.size();
    __f.resize(base + __levels * (128 + __fanout * b::NESTED_OFFSET_SIZE));

    ::Iris::File::Offset cursor = base, child = 0;
    for (std::size_t i = 0; i < __levels; ++i) {
        std::vector<b::AttributeSizeEntry> e(1);
        if (i == 0) {
            e[0] = {.key = "k", .value = "leaf"};
        } else {
            e[0] = {.key    = "k",
                    .nested = std::vector<::Iris::File::Offset>(__fanout, child),
                    .KIND   = k::AttributeKinds::ATTRIBUTE_NESTED};
        }
        const b::AttributeSizesCreateInfo si{.entries = e};
        const b::AttributeBytesCreateInfo bi{.entries = e};
        const ::Iris::File::Offset s_at = cursor; cursor += b::size_of(si);
        const ::Iris::File::Offset b_at = cursor; cursor += b::size_of(bi);
        const ::Iris::File::Offset a_at = cursor; cursor += b::ATTRIBUTES::header_size;
        IFE_CHECK(static_cast<bool>(b::store(__f.data(), s_at, si)));
        IFE_CHECK(static_cast<bool>(b::store(__f.data(), b_at, bi)));
        IFE_CHECK(static_cast<bool>(b::store(__f.data(), a_at, b::AttributesCreateInfo{
            .FORMAT = k::MetadataFormats::METADATA_DICOM, .VERSION = 2024,
            .SIZES_OFFSET = s_at, .BYTES_OFFSET = b_at})));
        child = a_at;
    }
    __f.resize(cursor);
    ::Iris::File::store<std::uint64_t>(__f.data() + b::FILE_HEADER::offset::FILE_SIZE, __f.size());
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
    namespace b = ::Iris::File::blocks;
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    const ::Iris::File::Offset head = append_attribute_chain(f, b::MAX_BLOCK_DEPTH - 4, 40);
    BYTE* value = first_nested_value(f, root_attributes(f));
    IFE_CHECK(value != nullptr);
    if (!value) return;
    ::Iris::File::store<std::uint64_t>(value, head);

    // Accepted, not merely survived: the file is well formed, and a reader
    // that rejected sharing would be refusing something the format allows.
    const auto result = Iris::File::validate_file_structure({f.data(), f.size()});
    IFE_CHECK(result == Iris::IRIS_SUCCESS);
    if (result != Iris::IRIS_SUCCESS) std::fprintf(stderr, "  %s\n", result.message.c_str());

    // The map walks the same edges on files with no validated graph behind
    // them, so it carries the same memory.
    const auto map = Iris::File::generate_file_map({f.data(), f.size()});
    IFE_CHECK(map.size() > 0);
}

void test_attribute_nesting_depth_is_bounded() {
    namespace b = ::Iris::File::blocks;
    namespace k = ::Iris::File::constants;
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    // Longer than the runtime's own bound, which is derived from the block
    // graph's limit -- so this cannot drift from the constant it tests.
    const std::size_t chain = b::MAX_BLOCK_DEPTH;
    const ::Iris::File::Offset base = f.size();
    f.resize(base + chain * 128);

    ::Iris::File::Offset cursor = base, child = 0;
    for (std::size_t i = 0; i < chain; ++i) {
        std::vector<b::AttributeSizeEntry> e(1);
        if (i == 0) e[0] = {.key = "k", .value = "leaf"};
        else        e[0] = {.key = "k", .nested = {child},
                            .KIND = k::AttributeKinds::ATTRIBUTE_NESTED};
        const b::AttributeSizesCreateInfo si{.entries = e};
        const b::AttributeBytesCreateInfo bi{.entries = e};
        const ::Iris::File::Offset s_at = cursor; cursor += b::size_of(si);
        const ::Iris::File::Offset b_at = cursor; cursor += b::size_of(bi);
        const ::Iris::File::Offset a_at = cursor; cursor += b::ATTRIBUTES::header_size;
        IFE_CHECK(static_cast<bool>(b::store(f.data(), s_at, si)));
        IFE_CHECK(static_cast<bool>(b::store(f.data(), b_at, bi)));
        IFE_CHECK(static_cast<bool>(b::store(f.data(), a_at, b::AttributesCreateInfo{
            .FORMAT = k::MetadataFormats::METADATA_DICOM, .VERSION = 2024,
            .SIZES_OFFSET = s_at, .BYTES_OFFSET = b_at})));
        child = a_at;
    }
    f.resize(cursor);
    ::Iris::File::store<std::uint64_t>(f.data() + b::FILE_HEADER::offset::FILE_SIZE, f.size());

    // Hang the chain off the root, replacing the fixture's own first item.
    const auto attrs = root_attributes(f);
    BYTE* value = first_nested_value(f, attrs);
    IFE_CHECK(value != nullptr);
    if (!value) return;
    ::Iris::File::store<std::uint64_t>(value, child);

    const auto result = Iris::File::validate_file_structure({f.data(), f.size()});
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
    try { (void)Iris::File::abstract_file_structure({f.data(), f.size()}); }
    catch (const std::runtime_error&) { threw = true; }
    IFE_CHECK(threw);
}

void test_file_map_finds_every_block() {
    v1_fixture::Expected expected;
    auto f = v1_slide(expected);

    const auto map = Iris::File::generate_file_map({f.data(), f.size()});
    IFE_CHECK(map.file_size == expected.file_size);

    // Ordered by offset, which is the property the whole API exists for:
    // "what lies after the byte I am about to overwrite".
    IFE_CHECK(map.count(0) == 1);
    IFE_CHECK(map.at(0).type == Iris::File::Abstraction::MAP_ENTRY_FILE_HEADER);

    auto has = [&map](Iris::File::Abstraction::MapEntryType type) {
        for (const auto& [offset, entry] : map) if (entry.type == type) return true;
        return false;
    };
    using namespace Iris::File::Abstraction;
    for (auto type : {MAP_ENTRY_TILE_TABLE, MAP_ENTRY_METADATA, MAP_ENTRY_LAYER_EXTENTS,
                      MAP_ENTRY_TILE_OFFSETS, MAP_ENTRY_ATTRIBUTES, MAP_ENTRY_ATTRIBUTE_SIZES,
                      MAP_ENTRY_ATTRIBUTE_BYTES, MAP_ENTRY_ICC_PROFILE,
                      MAP_ENTRY_IMAGES, MAP_ENTRY_IMAGE_BYTES,
                      MAP_ENTRY_ANNOTATIONS, MAP_ENTRY_ANNOTATION_BYTES})
        IFE_CHECK(has(type));

    int blocks = 0, tile_data = 0;
    for (const auto& [offset, entry] : map) {
        IFE_CHECK(offset + entry.size <= expected.file_size);
        if (entry.type == Iris::File::Abstraction::MAP_ENTRY_TILE_PIXEL_DATA) ++tile_data;
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

// ---- reference_fields_view: the table the recovery census walks (RB-2) ---- //

namespace k = ::Iris::File::constants;
namespace p = ::Iris::File::primitives;
using ::Iris::File::Offset;
using ::Iris::File::Size;
using ::Iris::File::Abstraction::FieldInfo;

struct TableWalk {
    const std::vector<BYTE>&                   file;
    std::uint32_t                              version = 0;
    std::set<Offset>                           reached{0};
    std::vector<std::pair<Offset, k::RecoveryCodes>> todo{{0, k::RecoveryCodes::RECOVER_FILE_HEADER}};
    std::set<std::string>&                     used;  ///< "OWNER.FIELD" of every non-null field seen
};

/// Every seat of `field` in the block at `at`: one, or one per array entry.
/// On a clean file both witnesses of each edge hold, so a wrong field_offset
/// reads a word that names no block of the declared type.
void follow(TableWalk& w, Offset at, k::RecoveryCodes owner, const FieldInfo& field) {
    const BYTE* base   = w.file.data();
    const Size  copies = field.in_entry ? ::Iris::File::load<std::uint32_t>(base + at + p::ArrayHeader::COUNT) : 1;
    const Size  stride = field.in_entry ? ::Iris::File::load<std::uint16_t>(base + at + p::ArrayHeader::STRIDE) : 0;
    for (Size i = 0; i < copies; ++i) {
        const Offset child = ::Iris::File::load<std::uint64_t>(base + at + field.field_offset + i * stride);
        if (child == k::NULL_OFFSET) {
            IFE_CHECK(field.nullable);
            continue;
        }
        IFE_CHECK(child < w.file.size());
        if (child >= w.file.size()) return;
        IFE_CHECK(p::BlockHeader::validation_at(base, child) == child);
        IFE_CHECK(p::BlockHeader::recovery_at(base, child) == static_cast<std::uint16_t>(field.child_recovery));
        w.used.insert(std::to_string(static_cast<unsigned>(owner)) + "." + field.name);
        if (w.reached.insert(child).second) w.todo.emplace_back(child, field.child_recovery);
    }
}

/// The census will walk the offset graph from reference_fields_view() alone,
/// so the table must name every seat the generated accessors read. Walked from
/// the root over each corpus file: every non-null word names a block whose
/// VALIDATION is its own offset and whose tag is the field's child_recovery,
/// and every block the table reaches is one generate_file_map finds. What the
/// map finds and the table does not must be what the table cannot see by
/// design: tile streams, and structures reached through nested attribute
/// values. Across the corpus every field must name a real child at least
/// once, or its offset was never tested.
void test_reference_fields_name_every_edge() {
    std::set<std::string> used;
    for (const char* name : {"cipher_iris.test_slide", "v1_0_witness.test_slide",
                             "v1_1_witness.test_slide"}) {
        const std::vector<BYTE> f = ife_load_fixture(g_corpus_dir, name);
        IFE_CHECK(!f.empty());
        if (f.empty()) continue;
        TableWalk w{.file = f, .used = used};
        w.version = ::Iris::File::versioned_root(f.data(), f.size()).__version;
        while (!w.todo.empty()) {
            const auto [at, owner] = w.todo.back();
            w.todo.pop_back();
            for (const FieldInfo& field : Iris::File::Abstraction::reference_fields_view(owner))
                if (field.since <= w.version) follow(w, at, owner, field);
        }

        using namespace Iris::File::Abstraction;
        const FileMap map = Iris::File::generate_file_map({f.data(), f.size()});
        for (const Offset at : w.reached) IFE_CHECK(map.count(at) == 1);
        for (const auto& [at, entry] : map) {
            if (w.reached.count(at) || entry.type == MAP_ENTRY_TILE_PIXEL_DATA) continue;
            const bool nested = entry.type == MAP_ENTRY_ATTRIBUTES ||
                                entry.type == MAP_ENTRY_ATTRIBUTE_SIZES ||
                                entry.type == MAP_ENTRY_ATTRIBUTE_BYTES;
            if (!nested) std::fprintf(stderr, "  %s: the table never reaches the block at %llu\n",
                                      name, static_cast<unsigned long long>(at));
            IFE_CHECK(nested);
        }
    }

    std::size_t fields = 0;
    for (unsigned tag = 0x5500; tag <= 0x55FF; ++tag)
        fields += Iris::File::Abstraction::reference_fields_view(static_cast<k::RecoveryCodes>(tag)).size();
    if (used.size() != fields)
        std::fprintf(stderr, "  the corpus exercises %zu of the table's %zu fields\n", used.size(), fields);
    IFE_CHECK(fields == 16);   // the spec's points_to edges: RB-2
    IFE_CHECK(used.size() == fields);
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
    test_reference_fields_name_every_edge();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_runtime_tests: all checks passed\n");
    return 0;
}
