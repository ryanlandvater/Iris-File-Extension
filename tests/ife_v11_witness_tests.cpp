/**
 * @file ife_v11_witness_tests.cpp
 * @brief The 1.1 witness read back: every 1.1 field present-and-correct on
 * the 1.1 witness, absent on the 1.0 snapshot.
 *
 * The 1.1 witness was pinned by digest and read by nobody: the fields 1.1
 * added — TILE_LENGTH, layer Z_PLANES, per-stream frame Z_PLANES,
 * MICRONS_PLANE, the clinical stream, the sparse NULL_TILE slot — were
 * evidence, not assertions. A digest pin catches a change; it does not catch
 * a reader that stops returning them correctly. This test is the 1.1 half of
 * the pair ife_v1_oracle_tests is for 1.0, and the version-gated reads it
 * makes are the same guarantee in both directions: a 1.1 field that reads
 * wrong on the 1.1 witness, or that reads as present on a 1.0 file, fails
 * here.
 */
#include "IFE_Blocks.hpp"
#include "ife_corpus_path.hpp"
#include "ife_v11_fixture.hpp"

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

using ::IFE::BYTE;
namespace b = ::IFE::blocks;
namespace k = ::IFE::constants;

std::vector<BYTE> read_whole_file(const std::string& __path) {
    std::FILE* in = std::fopen(__path.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "ife_v11_witness_tests: could not open %s\n", __path.c_str());
        return {};
    }
    std::vector<BYTE> bytes;
    std::fseek(in, 0, SEEK_END);
    const long n = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    if (n > 0) bytes.resize(static_cast<std::size_t>(n));
    const auto read = std::fread(bytes.data(), 1, bytes.size(), in);
    std::fclose(in);
    if (read != bytes.size()) {
        std::fprintf(stderr, "ife_v11_witness_tests: short read from %s\n", __path.c_str());
        return {};
    }
    return bytes;
}

// ---- the 1.1 witness: every 1.1 field present and correct ---------------- //
void test_11_fields_present(const std::vector<BYTE>& f, const v11_fixture::Expected& e) {
    BYTE* p = const_cast<BYTE*>(f.data());

    // Bootstrap at the declared version, exactly as a decoder must: the
    // root's own version is unknowable until it has been read.
    const b::FILE_HEADER bootstrap{p, 0, f.size(), UINT32_MAX};
    const std::uint32_t  declared =
        (static_cast<std::uint32_t>(bootstrap.extension_major()) << 16) |
        bootstrap.extension_minor();
    IFE_CHECK(declared == 0x00010001u);          // the witness declares 1.1
    IFE_CHECK(declared == b::VERSION_WRITTEN);   // and 1.1 is this build's version

    const b::FILE_HEADER root{p, 0, f.size(), declared};
    IFE_CHECK(static_cast<bool>(root));

    // The whole graph, at the declared version — clinical metadata included,
    // which a 1.0 walk could not follow.
    const auto deep = root.validate_deep();
    IFE_CHECK(static_cast<bool>(deep));
    if (!deep)
        std::fprintf(stderr, "  deep validation of the 1.1 witness failed in %s.%s\n",
                     deep.block, deep.field);

    IFE_CHECK(root.file_revision() == e.revision);

    // ---- tile table ------------------------------------------------------ //
    const auto tt = root.tile_table_offset();
    IFE_CHECK(static_cast<bool>(tt));
    IFE_CHECK(tt.encoding() == k::TileEncodings::TILE_ENCODING_JPEG);
    IFE_CHECK(tt.format()   == k::PixelFormats::FORMAT_R8G8B8A8);
    IFE_CHECK(tt.x_extent() == e.x_extent);
    IFE_CHECK(tt.y_extent() == e.y_extent);
    // TILE_LENGTH is the 1.1 field: present, and not the 256 that zero and
    // absence both mean. A decoder returning the default passes every other
    // gate; only this assertion notices.
    const auto length = tt.tile_length();
    IFE_CHECK(length.has_value());
    IFE_CHECK(length && *length == e.tile_length);
    IFE_CHECK(!static_cast<bool>(tt.cipher_offset()));   // witness carries none

    // ---- layer extents ---------------------------------------------------- //
    const auto le = tt.layer_extents_offset();
    IFE_CHECK(le.count() == e.layers.size());
    IFE_CHECK(le.stride() == b::LAYER_EXTENTS::LAYER_EXTENT::entry_size);
    for (std::size_t i = 0; i < e.layers.size(); ++i) {
        const auto& spec  = e.layers[i];
        const auto  entry = le.entry(static_cast<std::uint32_t>(i));
        IFE_CHECK(entry.x_tiles() == spec.x_tiles);
        IFE_CHECK(entry.y_tiles() == spec.y_tiles);
        IFE_CHECK(entry.scale()   == spec.scale);
        // Z_PLANES: the 1.1 field, at a value > 1 — the case no older
        // fixture exercises.
        const auto planes = entry.z_planes();
        IFE_CHECK(planes.has_value());
        IFE_CHECK(planes && *planes == spec.z_planes);
    }

    // ---- tile offsets: one sparse slot ------------------------------------- //
    const auto to = tt.tile_offsets_offset();
    IFE_CHECK(to.count() == e.tile_count);
    IFE_CHECK(to.stride() == b::TILE_OFFSETS::TILE_OFFSET::entry_size);
    std::uint32_t nulls = 0;
    for (std::uint32_t i = 0; i < e.tile_count; ++i) {
        if (i == e.null_tile_index) {
            IFE_CHECK(to.entry(i).offset() == v11_fixture::NULL_TILE);
            IFE_CHECK(to.entry(i).size_field() == 0);
            ++nulls;
            continue;
        }
        IFE_CHECK(to.entry(i).offset() != v11_fixture::NULL_TILE);
        IFE_CHECK(to.entry(i).size_field() == e.tile_stream_bytes);
        // Streams are laid out frame-then-bytes, so consecutive non-null
        // streams sit header_size + stream bytes apart.
        if (i > 0 && i - 1 != e.null_tile_index)
            IFE_CHECK(to.entry(i).offset() ==
                      to.entry(i - 1).offset() + b::TILE_PIXEL_DATA::header_size +
                          e.tile_stream_bytes);
    }
    IFE_CHECK(nulls == 1);
    // The file ends with the last stream, head to tail, no padding.
    IFE_CHECK(to.entry(e.null_tile_index - 1).offset() + e.tile_stream_bytes == f.size());

    // ---- frames and pixel bytes --------------------------------------------- //
    // A frame is addressed backward from the stream it precedes: the handle
    // is constructed at the stream offset the entry names, exactly as the
    // runtime's recovery scan builds it.
    std::size_t frames_seen = 0;
    for (std::uint32_t i = 0; i < e.tile_count; ++i) {
        if (i == e.null_tile_index) continue;
        const auto stream_at = to.entry(i).offset();
        const b::TILE_PIXEL_DATA tile_frame{p, stream_at, f.size(), declared};
        IFE_CHECK(static_cast<bool>(tile_frame));
        const auto index = tile_frame.tile_index();
        IFE_CHECK(index.has_value());
        IFE_CHECK(index && *index == i);
        const auto planes = tile_frame.z_planes();
        IFE_CHECK(planes.has_value());
        IFE_CHECK(planes && *planes == e.frame_z_planes[frames_seen]);
        // The dummy pixel bytes, untouched.
        for (std::uint32_t j = 0; j < e.tile_stream_bytes; ++j)
            IFE_CHECK(p[stream_at + j] == 0xCD);
        ++frames_seen;
    }
    IFE_CHECK(frames_seen == e.frame_z_planes.size());

    // ---- metadata ----------------------------------------------------------- //
    const auto md = root.metadata_offset();
    IFE_CHECK(md.codec_major() == e.codec_major);
    IFE_CHECK(md.codec_minor() == e.codec_minor);
    IFE_CHECK(md.codec_build() == e.codec_build);
    IFE_CHECK(md.microns_pixel() == e.microns_pixel);
    IFE_CHECK(md.magnification() == e.magnification);
    // MICRONS_PLANE: the 1.1 field.
    const auto plane = md.microns_plane();
    IFE_CHECK(plane.has_value());
    IFE_CHECK(plane && *plane == e.microns_plane);

    // ---- clinical stream ----------------------------------------------------- //
    const auto clinical = md.clinical_offset();
    IFE_CHECK(static_cast<bool>(clinical));
    const auto clinical_encoding = clinical.encoding();
    IFE_CHECK(clinical_encoding.has_value());
    IFE_CHECK(clinical_encoding &&
              *clinical_encoding == k::ClinicalEncodings::CLINICAL_HL7_V2);
    const auto clinical_bytes = clinical.bytes();
    IFE_CHECK(clinical_bytes.size == e.clinical_bytes.size());
    IFE_CHECK(std::memcmp(clinical_bytes.data, e.clinical_bytes.data(),
                          e.clinical_bytes.size()) == 0);

    // ---- attributes ----------------------------------------------------------- //
    const auto attrs = md.attributes_offset();
    IFE_CHECK(static_cast<bool>(attrs));
    IFE_CHECK(attrs.format() == k::MetadataFormats::METADATA_I2S);
    const auto attr_sizes = attrs.sizes_offset();
    IFE_CHECK(attr_sizes.count() == 1);
    IFE_CHECK(attr_sizes.entry(0).key_size()   == e.attribute_key.size());
    IFE_CHECK(attr_sizes.entry(0).value_size() == e.attribute_value.size());
    IFE_CHECK(attr_sizes.entry(0).kind() == k::AttributeKinds::ATTRIBUTE_STRING);
    const auto attr_run = attrs.bytes_offset().bytes();
    IFE_CHECK(std::memcmp(attr_run.data, e.attribute_key.data(),
                          e.attribute_key.size()) == 0);
    IFE_CHECK(std::memcmp(attr_run.data + e.attribute_key.size(),
                          e.attribute_value.data(), e.attribute_value.size()) == 0);

    // ---- ICC ------------------------------------------------------------------ //
    const auto icc = md.icc_color_offset();
    IFE_CHECK(static_cast<bool>(icc));
    const auto icc_bytes = icc.bytes();
    IFE_CHECK(icc_bytes.size == e.icc_bytes);
    IFE_CHECK(icc_bytes.data[0] == 0x01);

    // ---- associated images ------------------------------------------------------ //
    const auto im = md.images_offset();
    IFE_CHECK(im.count() == 1);
    IFE_CHECK(im.entry(0).width()  == e.image_width);
    IFE_CHECK(im.entry(0).height() == e.image_height);
    IFE_CHECK(im.entry(0).encoding() == k::ImageEncodings::IMAGE_ENCODING_JPEG);
    IFE_CHECK(im.entry(0).format()   == k::PixelFormats::FORMAT_R8G8B8A8);
    IFE_CHECK(im.entry(0).orientation() == e.image_orientation);
    const auto ib = im.entry(0).bytes_offset();
    IFE_CHECK(ib.title_size() == e.image_label.size());
    IFE_CHECK(ib.image_size() == e.image_stream_bytes);

    // ---- annotations ------------------------------------------------------------- //
    const auto an = md.annotations_offset();
    IFE_CHECK(static_cast<bool>(an));
    IFE_CHECK(an.count() == 1);
    const auto& spec  = e.annotation;
    const auto  entry = an.entry(0);
    IFE_CHECK(entry.identifier()   == spec.identifier);
    IFE_CHECK(entry.format()       == static_cast<k::AnnotationTypes>(spec.format));
    IFE_CHECK(entry.x_location()   == spec.xLocation);
    IFE_CHECK(entry.y_location()   == spec.yLocation);
    IFE_CHECK(entry.x_size()       == spec.xSize);
    IFE_CHECK(entry.y_size()       == spec.ySize);
    IFE_CHECK(entry.pixel_width()  == spec.width);
    IFE_CHECK(entry.pixel_height() == spec.height);
    IFE_CHECK(entry.parent_id()    == spec.parent);
    const auto payload = entry.bytes_offset().bytes();
    IFE_CHECK(payload.size == spec.payload.size());
    IFE_CHECK(std::memcmp(payload.data, spec.payload.data(), spec.payload.size()) == 0);

    // The named group: title and members sliced out of one shared byte run at
    // the boundary its sizes entry declares.
    const auto gs = an.group_sizes_offset();
    const auto gb = an.group_bytes_offset();
    IFE_CHECK(static_cast<bool>(gs));
    IFE_CHECK(static_cast<bool>(gb));
    IFE_CHECK(gs.count() == 1);
    IFE_CHECK(gs.entry(0).title_size()   == e.group.title.size());
    IFE_CHECK(gs.entry(0).member_count() == e.group.members.size());
    const auto    group_run = gb.bytes();
    ::IFE::Size   run_at    = 0;
    IFE_CHECK(std::memcmp(group_run.data + run_at, e.group.title.data(),
                          e.group.title.size()) == 0);
    run_at += e.group.title.size();
    for (const std::uint32_t id : e.group.members) {
        const ::IFE::BYTE* m = group_run.data + run_at;
        const std::uint32_t decoded =
            static_cast<std::uint32_t>(m[0]) |
            (static_cast<std::uint32_t>(m[1]) << 8) |
            (static_cast<std::uint32_t>(m[2]) << 16);
        IFE_CHECK(decoded == id);
        run_at += 3;
    }
    IFE_CHECK(run_at == group_run.size);
}

// ---- the 1.0 witness: every 1.1 field absent --------------------------------- //
void test_11_fields_absent_on_10(const std::vector<BYTE>& f) {
    BYTE* p = const_cast<BYTE*>(f.data());

    const b::FILE_HEADER bootstrap{p, 0, f.size(), UINT32_MAX};
    const std::uint32_t  declared =
        (static_cast<std::uint32_t>(bootstrap.extension_major()) << 16) |
        bootstrap.extension_minor();
    IFE_CHECK(declared == 0x00010000u);

    const b::FILE_HEADER root{p, 0, f.size(), declared};
    IFE_CHECK(static_cast<bool>(root));

    const auto tt = root.tile_table_offset();
    IFE_CHECK(tt.tile_length() == std::nullopt);

    // The snapshot's 2x2 / 4x4 / 8x8 layers: no plane count anywhere.
    const auto le = tt.layer_extents_offset();
    IFE_CHECK(le.count() == 3);
    for (std::uint32_t i = 0; i < le.count(); ++i)
        IFE_CHECK(le.entry(i).z_planes() == std::nullopt);

    const auto md = root.metadata_offset();
    IFE_CHECK(md.microns_plane() == std::nullopt);
    IFE_CHECK(!static_cast<bool>(md.clinical_offset()));

    // The 1.0 snapshot is dense: no sparse slot to read a NULL_TILE through.
    const auto to = tt.tile_offsets_offset();
    IFE_CHECK(to.count() == 84);
    for (std::uint32_t i = 0; i < to.count(); ++i)
        IFE_CHECK(to.entry(i).offset() != v11_fixture::NULL_TILE);

    // No frames: the version gate keeps the frame fields closed on 1.0 bytes.
    const b::TILE_PIXEL_DATA frame{p, to.entry(0).offset(), f.size(), declared};
    IFE_CHECK(frame.tile_index() == std::nullopt);
    IFE_CHECK(frame.z_planes()   == std::nullopt);
}

}  // namespace

int main(int argc, const char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 2;
    }
    // Bazel cannot pass a directory; BUILD.bazel passes the runfiles path of
    // one corpus file and its parent is the directory CTest passes directly.
    const std::string corpus_dir = ife_corpus_dir(argv[1]);

    v11_fixture::Expected expected;
    const auto bytes11 = v11_fixture::load_snapshot(
        corpus_dir + "/v1_1_witness.test_slide", expected);
    if (bytes11.empty()) {
        std::fprintf(stderr, "ife_v11_witness_tests: no 1.1 witness in %s "
                             "(the corpus fetch runs at configure; see "
                             "tests/corpus/manifest.json)\n", corpus_dir.c_str());
        return 1;
    }
    test_11_fields_present(bytes11, expected);

    const auto bytes10 = read_whole_file(corpus_dir + "/v1_0_witness.test_slide");
    if (bytes10.empty()) {
        std::fprintf(stderr, "ife_v11_witness_tests: no 1.0 snapshot in %s\n",
                     corpus_dir.c_str());
        return 1;
    }
    test_11_fields_absent_on_10(bytes10);

    if (g_failures) {
        std::fprintf(stderr, "ife_v11_witness_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ife_v11_witness_tests: 1.1 fields present on the 1.1 witness, "
                "absent on the 1.0 snapshot\n");
    return 0;
}
