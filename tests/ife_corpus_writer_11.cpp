/**
 * @file ife_corpus_writer_11.cpp
 * @brief Writes the 1.1 corpus fixture: the blocks no other fixture contains.
 *
 * **Why this is not part of ife_snapshot_writer.cpp.** That writer is built
 * against the *1.0 baseline* schema (tests/fixtures/build_baseline_spec.py),
 * deliberately, so the snapshot is 1.0 bytes a 1.1 reader has to cope with,
 * and its content is pinned field-for-field to tests/ife_v1_fixture.hpp
 * because the oracle reads its expectations from there. Neither property
 * survives adding 1.1 content to it. So this is a second writer, built
 * against the *current* generated layer, and it owns its own content.
 *
 * **This is the 1.1 witness: it carries every block the 1.1 specification
 * defines**, so the version's whole structural surface is pinned by one file.
 * Witnesses grow as a set, never individually -- each version gets its own,
 * frozen when that version ratifies, and the newest one doubles as the
 * feature corpus while its version is still draft.
 *
 * One block cannot be here. CIPHER is reachable only when TILE_TABLE.ENCODING
 * is TILE_ENCODING_IRIS (spec 2.3.2), which is reserved and unused, so a
 * witness carrying it would have to claim an encoding no encoder produces.
 * `--cipher` emits that file separately instead: same layout, IRIS encoding,
 * a CIPHER block present. The block is a bare universal header by design --
 * "encoders write nothing beyond the universal header and readers ignore its
 * contents" -- so nothing about its payload is invented.
 *
 * The tile frame is the reason the file is laid out the way it is. A frame is
 * addressed *backward* from the byte the tile-offsets entry names: its
 * VALIDATION word sits five bytes before the stream and stores its own
 * position, and the whole 11-byte header ends where the stream begins. So a
 * framed tile needs header_size bytes reserved ahead of every stream, and the
 * entry must address the stream, never the frame.
 *
 * Content is deliberately small: this is structural evidence, not a picture.
 *
 * Usage: ife_corpus_writer_11 <output-path> [--cipher]
 */
#include "IFE_Blocks.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace b = ::IFE::blocks;
namespace k = ::IFE::constants;

using ::IFE::BYTE;
using ::IFE::Offset;
using ::IFE::Size;

/// Dummy pixel bytes per tile. Real extent rather than zero length because
/// deep validation requires every addressed range to lie inside the file.
constexpr std::uint32_t TILE_BYTES = 16;

/// One HL7 v2 message, minimal but well-formed enough to be recognisable:
/// the block declares CLINICAL_HL7_V2, and a reader selects its parser from
/// that declaration rather than by sniffing these bytes.
const std::string HL7 =
    "MSH|^~\\&|IRIS|LAB|EMR|HOSP|20260818120000||ADT^A08|1|P|2.5\r";

/// The annotation group: a title and the identifiers of its members. The
/// sizes array slices the byte run, exactly as the attribute arrays do.
const std::string GROUP_TITLE = "Tumor front";
constexpr std::uint32_t GROUP_MEMBERS[] = {1, 2};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output-path> [--cipher]\n", argv[0]);
        return 2;
    }
    // The cipher variant: TILE_ENCODING_IRIS and a CIPHER block, which the
    // conformant JPEG witness cannot carry. Everything else is identical.
    const bool with_cipher = (argc > 2 && std::string(argv[2]) == "--cipher");

    // ---- semantic content -------------------------------------------------- //
    // Z_PLANES > 1 and a non-256 TILE_LENGTH are the 1.1 optional values that
    // sit at their defaults in every other fixture, so nothing exercises the
    // difference between "absent" and "present but one". The grid stays
    // coherent with the extent below: 2 tiles x 128 px = 256.
    const std::vector<b::LayerExtentEntry> extents = {
        {.X_TILES = 2, .Y_TILES = 2, .SCALE = 1.0f, .Z_PLANES = 3},
    };
    std::uint32_t tile_count = 0;
    for (const auto& e : extents) tile_count += e.X_TILES * e.Y_TILES;

    const std::string annotation_payload = "SVG-ish annotation bytes";
    const std::string attribute_key   = "SeriesDescription";
    const std::string attribute_value = "IFE 1.1 witness";
    const std::string image_label     = "thumbnail";
    const std::vector<BYTE> image_stream(48, 0xAB);
    const std::string icc(64, '\x01');

    // ---- lay the file out, leaves first ------------------------------------ //
    Offset at = 0;
    auto place = [&at](Size bytes) { const Offset here = at; at += bytes; return here; };

    b::LayerExtentsCreateInfo extents_info{.entries = extents};
    std::vector<b::TileOffsetEntry> tiles(tile_count);
    b::TileOffsetsCreateInfo tiles_info{.entries = tiles};

    const Offset header_at  = place(b::FILE_HEADER::header_size);
    const Offset table_at   = place(b::TILE_TABLE::header_size);
    const Offset extents_at = place(b::size_of(extents_info));
    const Offset tiles_at   = place(b::size_of(tiles_info));
    const Offset meta_at    = place(b::METADATA::header_size);

    // The clinical stream: a byte-array block whose payload follows the header.
    b::ClinicalMetadataCreateInfo clinical_info{
        .ENCODING = k::ClinicalEncodings::CLINICAL_HL7_V2,
        .bytes    = reinterpret_cast<const BYTE*>(HL7.data()),
        .count    = HL7.size()};
    const Offset clinical_at = place(b::size_of(clinical_info));

    // One annotation, so the group has something real to name.
    const b::AnnotationBytesCreateInfo annbytes_info{
        .bytes = reinterpret_cast<const BYTE*>(annotation_payload.data()),
        .count = annotation_payload.size()};
    const Offset annbytes_at = place(b::size_of(annbytes_info));

    // The group arrays, placed before the ANNOTATIONS block that names them.
    const std::vector<b::AnnotationGroupSizeEntry> groups = {
        {.TITLE_SIZE   = static_cast<std::uint16_t>(GROUP_TITLE.size()),
         .MEMBER_COUNT = static_cast<std::uint32_t>(std::size(GROUP_MEMBERS))}};
    b::AnnotationGroupSizesCreateInfo group_sizes_info{.entries = groups};

    // The group byte run: the title, then one 24-bit identifier per member.
    // The sizes entry above is what says where the title ends and the
    // identifiers begin; this layer just carries the bytes.
    std::vector<BYTE> group_bytes;
    group_bytes.insert(group_bytes.end(), GROUP_TITLE.begin(), GROUP_TITLE.end());
    for (const std::uint32_t id : GROUP_MEMBERS) {
        group_bytes.push_back(static_cast<BYTE>(id & 0xFF));
        group_bytes.push_back(static_cast<BYTE>((id >> 8) & 0xFF));
        group_bytes.push_back(static_cast<BYTE>((id >> 16) & 0xFF));
    }
    b::AnnotationGroupBytesCreateInfo group_bytes_info{
        .bytes = group_bytes.data(), .count = group_bytes.size()};

    const Offset group_sizes_at = place(b::size_of(group_sizes_info));
    const Offset group_bytes_at = place(b::size_of(group_bytes_info));

    std::vector<b::AnnotationEntry> annotations = {
        {.IDENTIFIER   = 1,
         .BYTES_OFFSET = annbytes_at,
         .FORMAT       = k::AnnotationTypes::ANNOTATION_TEXT,
         .X_LOCATION   = 0.25f,
         .Y_LOCATION   = 0.25f,
         .X_SIZE       = 0.5f,
         .Y_SIZE       = 0.5f,
         .PIXEL_WIDTH  = 32,
         .PIXEL_HEIGHT = 32,
         .PARENT_ID    = k::NULL_ID}};
    b::AnnotationsCreateInfo annotations_info{
        .GROUP_SIZES_OFFSET = group_sizes_at,
        .GROUP_BYTES_OFFSET = group_bytes_at,
        .entries            = annotations};
    const Offset annotations_at = place(b::size_of(annotations_info));

    // The attribute arrays and the block that names them. One text pair is
    // enough to exercise the slicing; nesting is already pinned by the 1.0
    // witness, which carries three attribute structures.
    const std::vector<b::AttributeSizeEntry> attributes = {
        {.key = attribute_key, .value = attribute_value}};
    b::AttributeSizesCreateInfo attr_sizes_info{.entries = attributes};
    b::AttributeBytesCreateInfo attr_bytes_info{.entries = attributes};
    const Offset attr_sizes_at = place(b::size_of(attr_sizes_info));
    const Offset attr_bytes_at = place(b::size_of(attr_bytes_info));
    const Offset attributes_at = place(b::ATTRIBUTES::header_size);

    b::IccProfileCreateInfo icc_info{
        .bytes = reinterpret_cast<const BYTE*>(icc.data()), .count = icc.size()};
    const Offset icc_at = place(b::size_of(icc_info));

    // IMAGE_BYTES carries a trailing payload the schema does not describe --
    // the label then the stream -- so size_of covers the header alone.
    b::ImageBytesCreateInfo image_bytes_info{
        .TITLE_SIZE = static_cast<std::uint16_t>(image_label.size()),
        .IMAGE_SIZE = static_cast<std::uint32_t>(image_stream.size())};
    const Offset imgbytes_at = place(b::size_of(image_bytes_info)
                                     + image_label.size() + image_stream.size());
    std::vector<b::ImageEntry> images = {
        {.BYTES_OFFSET = imgbytes_at,
         .WIDTH        = 64,
         .HEIGHT       = 64,
         .ENCODING     = k::ImageEncodings::IMAGE_ENCODING_JPEG,
         .FORMAT       = k::PixelFormats::FORMAT_R8G8B8A8,
         .ORIENTATION  = 90.0f}};
    b::ImagesCreateInfo images_info{.entries = images};
    const Offset images_at = place(b::size_of(images_info));

    // Reachable only under TILE_ENCODING_IRIS (spec 2.3.2), hence the variant.
    const Offset cipher_at = with_cipher ? place(b::CIPHER::header_size)
                                         : k::NULL_OFFSET;

    // ---- framed tile streams ----------------------------------------------- //
    // Each tile gets its 11-byte frame reserved immediately before its stream.
    // place() hands out the frame's start; the stream begins where the frame
    // ends, and that is the byte the tile-offsets entry must address.
    // The reservation only: place() moves the write head past the 11 bytes the
    // frame occupies so nothing else lands there. The frame is addressed from
    // the stream that follows it, so the stream offset is what gets stored and
    // what the tile-offsets entry must name.
    // The last slot is left NULL_TILE: a sparse layer is the only thing that
    // exercises the sentinel, and every fixture before this one was dense.
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (i + 1 == tiles.size()) {
            tiles[i].OFFSET = k::NULL_TILE;
            tiles[i].SIZE   = 0;
            continue;
        }
        (void)place(b::TILE_PIXEL_DATA::header_size);
        tiles[i].OFFSET = place(TILE_BYTES);
        tiles[i].SIZE   = TILE_BYTES;
    }

    const Size file_size = at;
    std::vector<BYTE> f(static_cast<std::size_t>(file_size), 0);
    BYTE* p = f.data();

    // ---- write --------------------------------------------------------------//
    int failures = 0;
    auto wrote = [&failures](const char* what, const b::Status& status) {
        if (!status) {
            std::fprintf(stderr, "ife_corpus_writer_11: %s failed (check %d at %llu)\n",
                         what, static_cast<int>(status.code),
                         static_cast<unsigned long long>(status.at));
            ++failures;
        }
    };

    wrote("LAYER_EXTENTS", b::store(p, extents_at, extents_info));
    wrote("TILE_OFFSETS",  b::store(p, tiles_at, tiles_info));
    wrote("TILE_TABLE",    b::store(p, table_at, b::TileTableCreateInfo{
        .ENCODING             = with_cipher
                                    ? k::TileEncodings::TILE_ENCODING_IRIS
                                    : k::TileEncodings::TILE_ENCODING_JPEG,
        .FORMAT               = k::PixelFormats::FORMAT_R8G8B8A8,
        .CIPHER_OFFSET        = cipher_at,
        .TILE_OFFSETS_OFFSET  = tiles_at,
        .LAYER_EXTENTS_OFFSET = extents_at,
        .X_EXTENT             = 256,
        .Y_EXTENT             = 256,
        // Deliberately not 256: zero and absence both mean the 256 default, so
        // a witness that leaves it unset cannot tell a decoder honouring the
        // field from one assuming the default.
        .TILE_LENGTH          = 128}));

    wrote("ANNOTATION_BYTES", b::store(p, annbytes_at, annbytes_info));
    wrote("ANNOTATION_GROUP_SIZES", b::store(p, group_sizes_at, group_sizes_info));
    wrote("ANNOTATION_GROUP_BYTES", b::store(p, group_bytes_at, group_bytes_info));
    wrote("ANNOTATIONS", b::store(p, annotations_at, annotations_info));

    wrote("CLINICAL_METADATA", b::store(p, clinical_at, clinical_info));

    wrote("ATTRIBUTE_SIZES", b::store(p, attr_sizes_at, attr_sizes_info));
    wrote("ATTRIBUTE_BYTES", b::store(p, attr_bytes_at, attr_bytes_info));
    wrote("ATTRIBUTES",      b::store(p, attributes_at, b::AttributesCreateInfo{
        .FORMAT       = k::MetadataFormats::METADATA_I2S,
        .VERSION      = 1,
        .SIZES_OFFSET = attr_sizes_at,
        .BYTES_OFFSET = attr_bytes_at}));

    wrote("ICC_PROFILE", b::store(p, icc_at, icc_info));

    wrote("IMAGE_BYTES", b::store(p, imgbytes_at, image_bytes_info));
    BYTE* payload = p + imgbytes_at + b::IMAGE_BYTES::header_size;
    std::memcpy(payload, image_label.data(), image_label.size());
    std::memcpy(payload + image_label.size(), image_stream.data(), image_stream.size());
    wrote("IMAGES", b::store(p, images_at, images_info));

    if (with_cipher)
        wrote("CIPHER", b::store(p, cipher_at, b::CipherCreateInfo{}));

    wrote("METADATA", b::store(p, meta_at, b::MetadataCreateInfo{
        .CODEC_MAJOR        = 1,
        .CODEC_MINOR        = 1,
        .CODEC_BUILD        = 0,
        .ATTRIBUTES_OFFSET  = attributes_at,
        .IMAGES_OFFSET      = images_at,
        .ICC_COLOR_OFFSET   = icc_at,
        .ANNOTATIONS_OFFSET = annotations_at,
        .MICRONS_PIXEL      = 0.25f,
        .MAGNIFICATION      = 40.0f,
        .CLINICAL_OFFSET    = clinical_at,
        .MICRONS_PLANE      = 0.5f}));

    wrote("FILE_HEADER", b::store(p, header_at, b::FileHeaderCreateInfo{
        .FILE_SIZE         = file_size,
        .FILE_REVISION     = 7,
        .TILE_TABLE_OFFSET = table_at,
        .METADATA_OFFSET   = meta_at}));

    // The frames, then the pixel bytes they precede. store() takes the frame's
    // START offset and writes backward-displaced fields forward from it, so
    // the VALIDATION word lands five bytes before the stream, as the layout
    // requires.
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        // store() takes the ANCHOR -- the first byte of the stream -- not the
        // frame's own start: TILE_PIXEL_DATA::offset::VALIDATION is -5, a
        // displacement *behind* the anchor, and the runtime builds the handle
        // the same way (IFE_Runtime.cpp: `frame{__base, stream_at, ...}`).
        // Passing frame_at here writes the frame five bytes early, which is
        // self-consistent enough for the recovery scan to accept while
        // attaching it to no stream at all.
        if (tiles[i].OFFSET == k::NULL_TILE) continue;   // no stream, no frame
        // Z_PLANES varies across streams on purpose: the layer declares a
        // maximum of 3, and an individual tile may carry fewer, so a reader
        // that takes the layer's maximum for the tile's own count is wrong in
        // a way only differing siblings expose. Zero denotes a single plane.
        wrote("TILE_PIXEL_DATA frame", b::store(p, tiles[i].OFFSET,
            b::TilePixelDataCreateInfo{
                .TILE_INDEX = static_cast<std::uint32_t>(i),
                .Z_PLANES   = static_cast<std::uint16_t>(i == 0 ? 3 : 0)}));
        std::memset(p + tiles[i].OFFSET, 0xCD, tiles[i].SIZE);
    }

    if (failures) return 1;

    std::FILE* out = std::fopen(argv[1], "wb");
    if (!out) {
        std::fprintf(stderr, "ife_corpus_writer_11: could not open %s\n", argv[1]);
        return 1;
    }
    const auto written = std::fwrite(f.data(), 1, f.size(), out);
    std::fclose(out);
    if (written != f.size()) {
        std::fprintf(stderr, "ife_corpus_writer_11: short write\n");
        return 1;
    }
    std::printf("ife_corpus_writer_11: %llu bytes -> %s\n",
                static_cast<unsigned long long>(file_size), argv[1]);
    return 0;
}
