/**
 * @file ife_recovery_tests.cpp
 * @brief The recovery census (MIGRATION.md RB-3), tested as FastFHIR tests its
 *        own (`../FastFHIR/tests/cpp/test_recovery.cpp`: check_clean_census,
 *        census_single_flip_sweep).
 *
 *   1. A clean file is one attached island: every block and stream hangs from
 *      the header, nothing is open, nothing is a hole, and the census's links
 *      are exactly what generate_file_map records.
 *   2. A newer writer's longer blocks leave gaps an older reader reads as
 *      VersionSkew, never as holes -- and the same bytes claiming this build's
 *      version read as holes, so it is the version that decides.
 *   3. Single flips, one at a time, over every bit of every fixture: every
 *      point the census opens is one the flip explains, and every decided link
 *      whose word or whose child's witnesses the flip hit is open.
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IFE_Builder.hpp"
#include "IFE_Recovery.hpp"
#include "IFE_Primitives.hpp"

#include "ife_builder_fixture.hpp"
#include "ife_corpus_path.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
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

using namespace Iris;
using namespace Iris::File;
namespace b = ::Iris::File::blocks;
namespace k = ::Iris::File::constants;
namespace p = ::Iris::File::primitives;

std::string g_corpus_dir;

Recovery::Census census_of(const std::vector<BYTE>& f) {
    return Recovery({f.data(), f.size()}).census();
}

const Recovery::Edge* edge_at(const Recovery::Census& c, Offset seat) {
    const auto it = std::lower_bound(c.edges.begin(), c.edges.end(), seat,
                                     [](const Recovery::Edge& e, Offset s) { return e.slot.seat < s; });
    return (it != c.edges.end() && it->slot.seat == seat) ? &*it : nullptr;
}

// ---- 1. a clean file is one attached island ------------------------------ //

void check_clean_census(const std::vector<BYTE>& f, const std::string& fixture) {
    const Recovery::Census c = census_of(f);
    const std::size_t streams = std::count_if(c.edges.begin(), c.edges.end(), [](const Recovery::Edge& e) {
        return e.slot.repr == Recovery::SlotRepr::TileEntry;
    });

    IFE_CHECK(c.anchors > 0);
    IFE_CHECK(c.islands.size() == 1);
    if (!c.islands.empty()) {
        IFE_CHECK(c.islands[0].attached && c.islands[0].root == 0);
        // Every block and every stream hangs from the header, the header included.
        IFE_CHECK(c.islands[0].members.size() == c.anchors + streams + 1);
    }
    IFE_CHECK(c.points.empty());
    IFE_CHECK(c.holes.empty());

    // The clean baseline: generate_file_map walks the same slots, so every
    // block and stream it records is the child of a decided link, and no link
    // names anything it does not record.
    const auto map = generate_file_map({f.data(), f.size()});
    std::set<Offset> children;
    for (const Recovery::Edge& e : c.edges) children.insert(e.child);
    std::size_t unlinked = 0;
    for (const auto& [off, entry] : map) unlinked += off != 0 && !children.contains(off);
    IFE_CHECK(unlinked == 0);
    IFE_CHECK(children.size() == map.size() - 1);
    if (!c.points.empty() || !c.holes.empty() || c.islands.size() != 1 || unlinked)
        std::fprintf(stderr, "  %s: %zu islands, %zu points, %zu holes, %zu unlinked map entries\n",
                     fixture.c_str(), c.islands.size(), c.points.size(), c.holes.size(), unlinked);
    std::printf("    %s: %zu blocks, %zu streams, %zu decided links\n", fixture.c_str(), c.anchors,
                streams, c.edges.size());
}

void test_census_clean_file_is_one_attached_island() {
    for (const char* name : {"cipher_iris.test_slide", "v1_0_witness.test_slide", "v1_1_witness.test_slide"})
        check_clean_census(ife_load_fixture(g_corpus_dir, name), name);
    check_clean_census(ife_builder_fixture(true), "Builder, frames");
    check_clean_census(ife_builder_fixture(false), "Builder, no frames");
}

// ---- 2. a newer writer's skew is not a hole ------------------------------ //

/// A slide as a newer writer lays it out: its FILE_HEADER and its METADATA are
/// each four bytes longer than this build knows. Built with the block tier,
/// claiming the extra bytes right after each, and stamped `major.minor`.
std::vector<BYTE> newer_slide(std::uint16_t major, std::uint16_t minor) {
    const Builder builder = Builder::create({.capacity = Size{1} << 20});
    (void)builder->claim(4);   // the header's appended field
    // The metadata first, so its run is bounded by the next block: a run at the
    // end of the file is slack, whatever trails it.
    const Offset meta_at = builder->append(b::MetadataCreateInfo{.MICRONS_PIXEL = 0.25f});
    (void)builder->claim(4);   // the metadata's appended field
    const Offset stream   = builder->claim(16);
    std::fill_n(builder->base() + stream, 16, BYTE{0xCD});
    const std::vector<b::TileOffsetEntry>  tiles   = {{.OFFSET = stream, .SIZE = 16}};
    const std::vector<b::LayerExtentEntry> extents = {{.X_TILES = 1, .Y_TILES = 1, .SCALE = 1.0f}};
    const Offset tiles_at   = builder->append(b::TileOffsetsCreateInfo{.entries = tiles});
    const Offset extents_at = builder->append(b::LayerExtentsCreateInfo{.entries = extents});
    const Offset table_at   = builder->append(b::TileTableCreateInfo{
        .ENCODING = k::TileEncodings::TILE_ENCODING_JPEG, .FORMAT = k::PixelFormats::FORMAT_R8G8B8A8,
        .TILE_OFFSETS_OFFSET = tiles_at, .LAYER_EXTENTS_OFFSET = extents_at,
        .X_EXTENT = 256, .Y_EXTENT = 256});
    builder->seal(b::FileHeaderCreateInfo{.EXTENSION_MAJOR = major, .EXTENSION_MINOR = minor,
                                          .TILE_TABLE_OFFSET = table_at, .METADATA_OFFSET = meta_at});
    return {builder->base(), builder->base() + builder.head()};
}

void test_newer_writer_skew_is_not_a_hole() {
    using Abstraction::GapClass;
    const std::vector<BYTE> newer = newer_slide(200, 0);
    const Recovery rec({newer.data(), newer.size()});
    const auto gaps = rec.scan().gaps;
    IFE_CHECK(gaps.size() == 2);
    for (const auto& g : gaps) IFE_CHECK(g.class_ == GapClass::VersionSkew);
    const Recovery::Census c = rec.census();
    IFE_CHECK(c.holes.empty());
    IFE_CHECK(c.points.empty());
    IFE_CHECK(c.islands.size() == 1);

    // The control: the same layout claiming this build's version. Nothing
    // explains the runs, so they are holes -- the version is what decides.
    const std::vector<BYTE> same = newer_slide(IFE_SCHEMA_VERSION_MAJOR, IFE_SCHEMA_VERSION_MINOR);
    const auto same_gaps = Recovery({same.data(), same.size()}).scan().gaps;
    IFE_CHECK(same_gaps.size() == 2);
    for (const auto& g : same_gaps) IFE_CHECK(g.class_ == GapClass::Hole);
}

// ---- 3. single flips ------------------------------------------------------ //

/// The bytes of a slot's word. A tile entry's word is its OFFSET and its SIZE.
bool word_holds(const Recovery::Slot& s, std::size_t byte) {
    return byte >= s.seat && byte < s.seat + 8;
}

/// The bytes of a slot's word a flip in which must open it. A tile entry's SIZE
/// is excluded: it is the stream's only length witness, so a SIZE that shrinks
/// contradicts nothing -- as FastFHIR's census does not police a string's LENGTH.
bool word_must_open(const Recovery::Slot& s, std::size_t byte) {
    const std::size_t width = s.repr == Recovery::SlotRepr::TileEntry ? 5 : 8;
    return byte >= s.seat && byte < s.seat + width;
}

void census_single_flip_sweep(std::vector<BYTE> f, const char* fixture) {
    const Recovery::Census c0 = census_of(f);
    const std::size_t n = f.size();
    if (c0.edges.empty()) {
        std::fprintf(stderr, "FAIL: %s: the clean census decides no links to sweep\n", fixture);
        ++g_failures;
        return;
    }
    const std::uint32_t version = versioned_root(f.data(), f.size()).__version;

    // Which decided link each byte witnesses, from the clean census: the link
    // whose child's witnesses hold the byte, and the link whose word holds it.
    // A block's witnesses are its VALIDATION and tag; a framed stream's, its
    // frame's VALIDATION and TILE_INDEX; an unframed stream has none.
    std::vector<Offset> child_header_of(n, k::NULL_OFFSET), word_of(n, k::NULL_OFFSET);
    for (const Recovery::Edge& e : c0.edges) {
        std::size_t from = e.child, to = e.child + p::BlockHeader::HEADER_SIZE;
        if (e.slot.repr == Recovery::SlotRepr::TileEntry) {
            const b::TILE_PIXEL_DATA frame{f.data(), e.child, f.size(), version};
            const bool framed = e.child >= b::TILE_PIXEL_DATA::header_size &&
                                frame.validation() == e.child + b::TILE_PIXEL_DATA::offset::VALIDATION;
            from = framed ? e.child + b::TILE_PIXEL_DATA::offset::TILE_INDEX : e.child;
            to   = e.child;
        }
        for (std::size_t j = from; j < to && j < n; ++j) child_header_of[j] = e.slot.seat;
        for (std::size_t j = e.slot.seat; j < e.slot.seat + 8 && j < n; ++j)
            if (word_must_open(e.slot, j)) word_of[j] = e.slot.seat;
    }
    // A nested value's seat is sliced by its ATTRIBUTE_SIZES, so a flip there
    // moves every nested slot of the byte array it slices.
    std::map<Offset, std::pair<Offset, Offset>> slicing;   // bytes block -> its sizes block's range
    for (const Recovery::Edge& e : c0.edges)
        if (e.slot.expect == k::RecoveryCodes::RECOVER_ATTRIBUTE_SIZES)
            for (const Recovery::Edge& y : c0.edges)
                if (y.slot.parent == e.slot.parent && y.slot.expect == k::RecoveryCodes::RECOVER_ATTRIBUTE_BYTES)
                    slicing[y.child] = {e.child, e.child + b::ATTRIBUTE_SIZES{f.data(), e.child, f.size(), version}.extent()};

    std::map<std::string, std::size_t> wrong;
    std::vector<std::string> examples;
    std::size_t opened = 0;
    const auto record = [&](const char* what, std::size_t byte, int bit, const std::string& detail) {
        ++wrong[what];
        if (examples.size() < 12)
            examples.push_back("byte " + std::to_string(byte) + " bit " + std::to_string(bit) + " " + detail);
    };
    const auto explained = [&](const Recovery::Census& c, const Recovery::Point& pt, std::size_t byte) {
        if (pt.kind == Recovery::PointKind::ArrayExtent)
            return byte >= pt.array + p::ArrayHeader::STRIDE && byte < pt.array + p::ArrayHeader::HEADER_SIZE;
        const Recovery::Edge* e = edge_at(c0, pt.slot.seat);
        if (word_holds(pt.slot, byte) || (e != nullptr && child_header_of[byte] == pt.slot.seat))
            return true;
        if (pt.slot.repr == Recovery::SlotRepr::Nested) {
            const auto s = slicing.find(pt.slot.parent);
            if (s != slicing.end() && byte >= s->second.first && byte < s->second.second)
                return true;
        }
        // A flipped word that lands exactly on another slot's child makes that
        // child claimed twice, and both slots open. A stream has an extent
        // only its entry claims, so for tile entries landing ON is overlapping:
        // a stream moved over its neighbour opens both.
        for (const Recovery::Point& q : c.points) {
            if (e == nullptr || q.kind != Recovery::PointKind::Open || q.slot.seat == pt.slot.seat ||
                !word_holds(q.slot, byte))
                continue;
            if (q.slot.stored == e->child)
                return true;
            if (q.slot.repr == Recovery::SlotRepr::TileEntry && e->slot.repr == Recovery::SlotRepr::TileEntry &&
                q.slot.stored < e->child + e->slot.claim && e->child < q.slot.stored + q.slot.claim)
                return true;
        }
        return false;
    };

    // The header's version decides how every block is read, so a flip there
    // reads the whole file under another layout. Deciding the version first is
    // the header's (RB-4), as FastFHIR restores its header before the census.
    const std::size_t version_from = b::FILE_HEADER::offset::EXTENSION_MAJOR;
    const std::size_t version_to   = b::FILE_HEADER::offset::FILE_REVISION;

    for (std::size_t byte = 0; byte < n; ++byte)
        for (int bit = 0; bit < 8; ++bit) {
            if (byte >= version_from && byte < version_to) continue;
            f[byte] ^= static_cast<BYTE>(1u << bit);
            const Recovery::Census c = census_of(f);
            f[byte] ^= static_cast<BYTE>(1u << bit);
            opened += c.points.size();

            for (const Recovery::Point& pt : c.points)
                if (!explained(c, pt, byte))
                    record("an unexplained point", byte, bit,
                           "opened seat " + std::to_string(pt.slot.seat) +
                               (pt.kind == Recovery::PointKind::ArrayExtent
                                    ? " (array " + std::to_string(pt.array) + ")" : ""));

            // Every decided link the flip damaged must be open. At an array's
            // entries the census may ask about the array instead.
            const auto asked = [&c](const Recovery::Edge& e) {
                for (const Recovery::Point& pt : c.points)
                    if ((pt.kind == Recovery::PointKind::Open && pt.slot.seat == e.slot.seat) ||
                        (pt.kind == Recovery::PointKind::ArrayExtent && pt.array == e.slot.parent))
                        return true;
                return false;
            };
            // The FILE_HEADER's own words are the header's to restore (RB-4).
            if (const Recovery::Edge* e = word_of[byte] != k::NULL_OFFSET ? edge_at(c0, word_of[byte]) : nullptr;
                e != nullptr && e->slot.parent != 0 && !asked(*e))
                record("a damaged slot left closed", byte, bit, "left seat " + std::to_string(e->slot.seat) + " closed");
            if (const Recovery::Edge* e = child_header_of[byte] != k::NULL_OFFSET ? edge_at(c0, child_header_of[byte]) : nullptr;
                e != nullptr && !asked(*e))
                record("a damaged child left closed", byte, bit, "left seat " + std::to_string(e->slot.seat) + " closed");
        }

    std::size_t total_wrong = 0;
    for (const auto& [what, count] : wrong) {
        std::printf("    %s: %zu x %s\n", fixture, count, what.c_str());
        total_wrong += count;
    }
    for (const std::string& e : examples) std::printf("      e.g. %s\n", e.c_str());
    std::printf("    %s: %zu single flips over %zu bytes, %zu points opened\n", fixture, n * 8, n, opened);
    IFE_CHECK(total_wrong == 0);
}

void test_census_single_flips() {
    for (const char* name : {"cipher_iris.test_slide", "v1_0_witness.test_slide", "v1_1_witness.test_slide"})
        census_single_flip_sweep(ife_load_fixture(g_corpus_dir, name), name);
    census_single_flip_sweep(ife_builder_fixture(true), "Builder, frames");
    census_single_flip_sweep(ife_builder_fixture(false), "Builder, no frames");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 2;
    }
    g_corpus_dir = ife_corpus_dir(argv[1]);

    test_census_clean_file_is_one_attached_island();
    test_newer_writer_skew_is_not_a_hole();
    test_census_single_flips();

    if (g_failures) {
        std::fprintf(stderr, "ife_recovery_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ife_recovery_tests: all checks passed\n");
    return 0;
}
