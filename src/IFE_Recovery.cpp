/**
 * @file IFE_Recovery.cpp
 * @brief The recovery census (MIGRATION.md RB-3).
 * @copyright Iris Developers, 2025-2026
 *
 * A port of FastFHIR's census (`../FastFHIR/src/FF_Recovery.cpp`, "PHASE 1",
 * and `recovery_algorithm_handoff.md` §6). It reads top to bottom as that file
 * does: policy and leaf reads, the board, the census, then the producers. The
 * two formats share the witnesses -- a block's VALIDATION holds its own offset
 * and its tag follows -- so the code is FastFHIR's wherever the wire is, and
 * says so where IFE's differs:
 *
 *   - The FILE_HEADER names its children through ordinary reference fields,
 *     so its slots come from slots_of() like every other block's.
 *   - A tile stream carries no header. scan() charges it to the tile offsets
 *     entry that names it, and its optional frame is its only witness.
 *   - Nested attribute values may share a structure (the format allows it), so
 *     they are exempt from H5.
 *   - A newer file's skew is recognised even for a block type that occurs
 *     once: IFE's growing blocks (FILE_HEADER, TILE_TABLE, METADATA) are
 *     single, where FastFHIR's run by the thousand.
 */
#include "IFE_Recovery.hpp"
#include "IFE_Primitives.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Iris::File {

namespace b = ::Iris::File::blocks;
namespace k = ::Iris::File::constants;
namespace p = ::Iris::File::primitives;
using Abstraction::FileMap;
using Abstraction::FileMapEntry;
using Abstraction::Gap;
using Abstraction::GapClass;

// =====================================================================
// TOP — policy, leaf reads, and the board
// =====================================================================
namespace {

constexpr Offset HEADER_PARENT = 0;  // the FILE_HEADER parents the tile table and the metadata

// ---- leaf reads ----

bool in_extent(uint64_t off, uint64_t width, Size size) noexcept {
    return off <= size && width <= size - off;
}

k::RecoveryCodes tag_at(const BYTE* base, Size size, uint64_t off) noexcept {
    return in_extent(off, p::BlockHeader::HEADER_SIZE, size)
               ? static_cast<k::RecoveryCodes>(load<std::uint16_t>(base + off + p::BlockHeader::RECOVERY))
               : k::RecoveryCodes::RECOVER_UNDEFINED;
}

bool has_magic(const BYTE* base, Size size) noexcept {
    return static_cast<bool>(versioned_root(base, size).validate());
}

// A newer writer may append fields this build has never seen, all
// legitimately, so the unfamiliar is evidence of damage only when this answers
// false. The version is the root's, composed as every block reads it.
bool file_is_newer(const BYTE* base, Size size) noexcept {
    return has_magic(base, size) && versioned_root(base, size).__version > b::VERSION_WRITTEN;
}

// ---- what a tag says about a block's layout ----

// Does this build know the layout of a block carrying `tag`?
bool sizable_tag(k::RecoveryCodes tag) noexcept {
    return Abstraction::entry_for(tag) != Abstraction::MAP_ENTRY_UNDEFINED;
}

// Whether blocks carrying `tag` are laid out as fields alone -- the one shape a
// newer writer lengthens, by appending fields. An array carries its stride and
// a byte array its count on the wire, so an older reader never sizes either
// short.
bool grows(k::RecoveryCodes tag) noexcept {
    return b::with_block(tag, nullptr, 0, 0, 0, false,
                         [](const auto& h) { return !requires { h.count(); }; });
}

// The provisional extent of the block at `off` under the tag it carries: what
// the census charges it when tiling the file. A stamped length that overruns
// the file falls back to the block's header.
FileMapEntry classify_block(const BYTE* base, Size size, uint32_t version, Offset off) {
    const k::RecoveryCodes tag  = tag_at(base, size, off);
    const Size             room = size - off;
    const Size extent = b::with_block(tag, base, off, size, version, Size{0}, [room](const auto& h) -> Size {
        const Size whole = h.extent();
        if (whole <= room) return whole;
        const Size header = std::remove_cvref_t<decltype(h)>::header_size;
        return header <= room ? header : Size{0};
    });
    return {Abstraction::entry_for(tag), off, extent};
}

// ---- the board: everything recovery knows, in RAM ----

// What a slot's bytes say, read on their own. FastFHIR also has Inline and
// Suspect, for slots whose word is a value or a pointer by a flag bit; IFE has
// no such slot.
enum class Reading : uint8_t {
    Absent,    // the slot is empty
    Intact,    // every witness of the link holds
    Dangling,  // the word names a position that is not a child of the expected type
};

struct Judged {
    Reading reading = Reading::Absent;
    Offset  target  = k::NULL_OFFSET;
};

// A block the board knows: one that vouches for itself, or one placed in a
// hole by chaining from the block before it, whose self-offset is the damage.
struct Anchor {
    k::RecoveryCodes tag  = k::RecoveryCodes::RECOVER_UNDEFINED;
    bool             lost = false;
    /// Whether it ends exactly where the next thing in the file begins. A real
    /// block does, because the file is dense where its extents are exact; a
    /// pointer word that a flip has turned into its own address self-validates
    /// too, and almost never does.
    bool             tiles = true;
};

struct Question {
    Recovery::Point point;
    bool            open = true;
};

struct Board {
    const BYTE* base       = nullptr;
    Size        size       = 0;
    uint32_t    version    = 0;
    bool        newer_file = false;
    std::map<Offset, Anchor>                   anchors;
    std::map<Offset, Size>                     cover;     // every block, stream and frame the scan charged, and each lost block, by start
    std::unordered_set<Offset>                 loose;     // charged streams that meet neither neighbour: no boundary
    std::vector<Gap>                           holes;     // ascending
    std::unordered_map<Offset, Recovery::Slot> owner;     // child -> its one parent's slot (H5)
    std::vector<Question>                      questions;
};

}  // namespace

// =====================================================================
// PHASE 1 — the census (recovery_algorithm_handoff.md §6)
// =====================================================================
namespace {

void   chain_hole(Board& B, const Gap& hole);
bool   abuts(const Board& B, uint64_t at, uint64_t length);
bool   meets_both(const Board& B, uint64_t at, uint64_t length);
Size   array_header(const Board& B, Offset off, k::RecoveryCodes tag);
Size   supported_extent(const Board& B, Offset off, k::RecoveryCodes tag);
bool   overlaps(const Board& B, uint64_t at, uint64_t length);
bool   read_slots(const Board& B, Offset off, k::RecoveryCodes tag, std::vector<Recovery::Slot>& out,
                  bool* refuted);
Judged judge(const Board& B, const Recovery::Slot& s);
std::unordered_map<Offset, std::vector<Offset>> children(const Board& B);
std::vector<Offset> attached(const Board& B, const std::unordered_map<Offset, std::vector<Offset>>& kids);
bool   ask(Board& B, const Recovery::Point& pt);

Board take_census(const BYTE* base, Size size, const FileMap& scan_map) {
    Board B{.base = base, .size = size, .version = versioned_root(base, size).__version,
            .newer_file = file_is_newer(base, size)};

    // The anchors and the holes come straight from the byte census. Each hole
    // is chained from its start, where the block before it ended, to place the
    // blocks whose self-offsets were destroyed.
    for (const auto& [off, entry] : scan_map) {
        B.cover.emplace(off, entry.size);
        if (entry.type != Abstraction::MAP_ENTRY_FILE_HEADER &&
            entry.type != Abstraction::MAP_ENTRY_TILE_PIXEL_DATA &&
            entry.type != Abstraction::MAP_ENTRY_TILE_FRAME)
            B.anchors.emplace(off, Anchor{Abstraction::recovery_for(entry.type), false, true});
    }
    std::unordered_set<uint64_t> skew_starts;
    for (const Gap& g : scan_map.gaps) {
        if (g.class_ == GapClass::VersionSkew)
            skew_starts.insert(g.start);
        else
            chain_hole(B, g);
        if (g.class_ == GapClass::Hole)
            B.holes.push_back(g);
    }
    // A block tiles when its end meets the next thing in the file -- a block
    // that vouches for itself, a lost one the chain placed there, a tile stream
    // or frame -- or the end of the file, or a newer writer's benign trailing
    // run. A gap is no evidence: gaps are measured from these same extents, so
    // an extent that is too short always leaves one starting exactly where it
    // ends.
    // Only a corroborated position is a boundary (FastFHIR counts only a block
    // that vouches for itself and tiles). A stream has no self-offset, so it
    // counts when it meets both neighbours, as every stream a writer laid down
    // does; one a damaged entry moved rarely meets even one. Decided first,
    // because the array extents below are measured against these boundaries.
    for (const auto& [off, entry] : scan_map)
        if (entry.type == Abstraction::MAP_ENTRY_TILE_PIXEL_DATA && !meets_both(B, off, entry.size))
            B.loose.insert(off);
    // An array covers what its bytes support, decided now that the streams
    // after it are charged: a stamp that damage inflated must not swallow them.
    for (const auto& [off, a] : B.anchors)
        if (array_header(B, off, a.tag) != 0)
            B.cover[off] = supported_extent(B, off, a.tag);
    for (auto& [off, a] : B.anchors) {
        const uint64_t end = off + B.cover.at(off);
        a.tiles = end >= size || skew_starts.contains(end) || B.cover.contains(end);
    }
    // A block that does not tile is sized by a tag that is not its own, or by a
    // damaged length, so beyond its signature its extent is no evidence.
    for (const auto& [off, a] : B.anchors)
        if (!a.tiles && !a.lost)
            B.cover[off] = p::BlockHeader::HEADER_SIZE;

    // Judge every slot: the header's first, then every block's under its own
    // wire tag -- a lost block's too, so its subtree hangs from it rather than
    // scattering into the orphan pools. Decided links keep this order, so each
    // parent's children come out in the order of its slots.
    std::vector<Recovery::Slot> slots;
    std::unordered_set<Offset>  refuted;
    if (has_magic(base, size))
        read_slots(B, HEADER_PARENT, k::RecoveryCodes::RECOVER_FILE_HEADER, slots, nullptr);
    for (const auto& [off, a] : B.anchors) {
        bool bad = false;
        if (read_slots(B, off, a.tag, slots, &bad) && bad)
            refuted.insert(off);
    }
    std::vector<Recovery::Edge>                             intact;
    std::unordered_map<Offset, size_t>                      claim;      // child -> its first claimant
    std::map<Offset, std::vector<size_t>>                   contested;  // child -> every claimant
    std::unordered_map<Offset, std::vector<Recovery::Slot>> open_of;
    for (const Recovery::Slot& s : slots) {
        const Judged j = judge(B, s);
        if (j.reading == Reading::Dangling)
            open_of[s.parent].push_back(s);
        if (j.reading != Reading::Intact)
            continue;
        const auto [it, first] = claim.emplace(j.target, intact.size());
        if (!first) {
            std::vector<size_t>& claimants = contested[j.target];
            if (claimants.empty())
                claimants.push_back(it->second);
            claimants.push_back(intact.size());
        }
        intact.push_back({s, j.target});
    }

    // A block two slots claim keeps neither claim: at most one of the two
    // words is right and the census cannot say which, so both open and a
    // cycle weighs them. A header slot keeps its claim, because it is the one
    // slot allowed to name its child, and the other word is the damage. Nested
    // attribute values may legitimately share a structure, so a child only
    // they claim keeps its first claim.
    std::vector<bool> dropped(intact.size(), false);
    for (const auto& [child, claimants] : contested) {
        const bool shared = std::all_of(claimants.begin(), claimants.end(), [&](size_t i) {
            return intact[i].slot.repr == Recovery::SlotRepr::Nested;
        });
        for (const size_t i : claimants) {
            if (shared || (i == claimants.front() && intact[i].slot.parent == HEADER_PARENT))
                continue;
            dropped[i] = true;
            open_of[intact[i].slot.parent].push_back(intact[i].slot);
        }
    }
    for (size_t i = 0; i < intact.size(); ++i)
        if (!dropped[i])
            B.owner.emplace(intact[i].child, intact[i].slot);

    // Only what the header reaches becomes a question. An orphaned subtree's
    // questions are answered when a cycle attaches it, as part of its branch.
    for (const Offset m : attached(B, children(B))) {
        if (const auto it = open_of.find(m); it != open_of.end())
            for (const Recovery::Slot& s : it->second)
                ask(B, {Recovery::PointKind::Open, s, k::NULL_OFFSET});
        if (refuted.contains(m))
            ask(B, {Recovery::PointKind::ArrayExtent, {}, m});
    }
    return B;
}

// Place the blocks a hole holds. The first lost block starts where the hole
// starts, and each one's residual tag gives the extent that places the next. A
// block whose self-offset took any number of flips is found this way; the
// chain stops at a tag this build cannot size. find_gaps() never classes a
// newer writer's skew as a hole, so the density this relies on holds.
void chain_hole(Board& B, const Gap& hole) {
    const uint64_t end = static_cast<uint64_t>(hole.start) + hole.length;
    for (uint64_t q = hole.start; q + p::BlockHeader::HEADER_SIZE <= end;) {
        if (!sizable_tag(tag_at(B.base, B.size, q)))
            return;
        const uint64_t extent = classify_block(B.base, B.size, B.version, static_cast<Offset>(q)).size;
        if (extent == 0 || q + extent > end)
            return;
        B.anchors.emplace(static_cast<Offset>(q), Anchor{tag_at(B.base, B.size, q), true, true});
        B.cover.emplace(static_cast<Offset>(q), extent);
        q += extent;
    }
}

// Where the first thing after `off` starts, or the end of the file. Where
// extents are exact this is where the block at `off` must end. A block counts
// only if it vouches for itself, carries a tag this build can size, and tiles:
// a pointer flipped into its own address self-validates too, and the bytes
// after it are neither a known tag nor the start of an extent that ends on the
// next block, as a real block's are. A stream or a frame counts as it is.
uint64_t next_block_after(const Board& B, uint64_t off) {
    for (auto it = B.cover.upper_bound(static_cast<Offset>(off)); it != B.cover.end(); ++it) {
        if (B.loose.contains(it->first))
            continue;
        const auto a = B.anchors.find(it->first);
        if (a == B.anchors.end() || (!a->second.lost && a->second.tiles && sizable_tag(a->second.tag)))
            return std::min<uint64_t>(it->first, B.size);
    }
    return B.size;
}

// ---- arrays: the geometry the bytes support ----

// The header an array of `tag` has before its entries.
Size array_header(const Board& B, Offset off, k::RecoveryCodes tag) {
    return b::with_block(tag, B.base, off, B.size, B.version, Size{0}, [](const auto& h) -> Size {
        if constexpr (requires { h.entries_begin(); h.stride(); }) return std::remove_cvref_t<decltype(h)>::header_size;
        return 0;   // a block, or a byte array: no entries
    });
}

ArrayGeometry stamped(const Board& B, Offset off) {
    return {load<std::uint16_t>(B.base + off + p::ArrayHeader::STRIDE),
            load<std::uint32_t>(B.base + off + p::ArrayHeader::COUNT)};
}

// Does the entry at `entry` name a child every witness of which holds? A word
// naming its own position is the block after the array read as an entry, and
// never one; nor is a block another slot already names (H5). An array whose
// entries name no block -- layer extents, attribute sizes, and tile offsets,
// whose streams have no witness of their own -- cannot answer, and says no.
bool names_an_entry(const Board& B, k::RecoveryCodes tag, Size header, uint64_t entry) {
    for (const Abstraction::FieldInfo& f : Abstraction::reference_fields_view(tag)) {
        if (!f.in_entry)
            continue;
        const uint64_t seat = entry + (f.field_offset - header);
        if (!in_extent(seat, sizeof(std::uint64_t), B.size))
            return false;
        const Offset child = load<std::uint64_t>(B.base + seat);
        const auto   a     = B.anchors.find(child);
        return a != B.anchors.end() && !a->second.lost && child != seat && !B.owner.contains(child) &&
               a->second.tag == f.child_recovery;
    }
    return false;
}

// The geometry the bytes around an array support, or nothing when they do not
// settle it (FastFHIR's derive). An array's entries carry no witnesses of their
// own, so its count is checked against the file instead: the entries end at the
// next block. The width is the one the file's version writes -- or, in a newer
// file, the stamp when it is wider, since a newer element may be longer and
// only the stamp says by how much. FastFHIR also special-cases an entry word
// flipped into its own address, which its scan records as a block; IFE's scan
// records only a block whose tag it knows, so no entry word ends the entries.
std::optional<ArrayGeometry> derive(const Board& B, Offset off, k::RecoveryCodes tag) {
    struct Shape { Size header = 0, entry = 0; };
    const Shape shape = b::with_block(tag, B.base, off, B.size, B.version, Shape{}, [&B](const auto& h) {
        if constexpr (requires { h.entries_begin(); h.stride(); }) {
            using Entry = std::remove_cvref_t<decltype(h.entry(0))>;
            return Shape{std::remove_cvref_t<decltype(h)>::header_size, Entry::entry_size_at(B.version)};
        }
        return Shape{};
    });
    const ArrayGeometry st      = stamped(B, off);
    const uint64_t      entries = static_cast<uint64_t>(off) + shape.header;
    if (shape.entry == 0 || !in_extent(entries, 0, B.size))
        return std::nullopt;
    const Size     width = B.newer_file ? std::max<Size>(st.stride, shape.entry) : shape.entry;
    const uint64_t limit = next_block_after(B, off);
    if (limit < entries)
        return std::nullopt;
    const uint64_t room = (limit - entries) / width;
    ArrayGeometry  g{width, st.count};
    if (st.count <= room) {
        // A count flipped low leaves entries past the stamped ones whose every
        // witness still holds.
        while (g.count < room && names_an_entry(B, tag, shape.header, entries + g.count * width))
            ++g.count;
        return g;
    }
    // A count flipped high overruns the next block. The entries must tile
    // exactly up to it, and the last of them must still name its child.
    const bool names = std::any_of(Abstraction::reference_fields_view(tag).begin(),
                                   Abstraction::reference_fields_view(tag).end(),
                                   [](const Abstraction::FieldInfo& f) { return f.in_entry; });
    const bool tiles      = (limit - entries) % width == 0;
    const bool last_holds = !names || room == 0 || names_an_entry(B, tag, shape.header, entries + (room - 1) * width);
    if (!tiles || !last_holds || room > UINT32_MAX)
        return std::nullopt;
    g.count = static_cast<uint32_t>(room);
    return g;
}

// What an array covers under the geometry its bytes support: its header and
// the entries derive() settles, or its header alone when nothing is settled.
// FastFHIR charges an array of blocks its header only, for the same reason: a
// stamp that damage inflated must not swallow what follows.
Size supported_extent(const Board& B, Offset off, k::RecoveryCodes tag) {
    const Size header = array_header(B, off, tag);
    const std::optional<ArrayGeometry> g = derive(B, off, tag);
    return g ? header + g->stride * g->count : header;
}

// ---- reading and judging slots ----

// Every slot of the block at `off`, read as `tag`, appended to `out` through
// the reading generate_file_map shares (slots_of). False when the reading is
// impossible under H3: a block never contains another block's first byte, so
// a block whose extent under `tag` would reach the next block is not a `tag`
// block, and reading it anyway lifts the next block's slots as this one's. A
// newer writer only ever makes a block longer than this build knows, so the
// true type is never refused by this. `refuted` reports an array whose stamped
// geometry the bytes around it contradict; its entries are then read under the
// geometry they support, no further than the stamp allows.
bool read_slots(const Board& B, Offset off, k::RecoveryCodes tag, std::vector<Recovery::Slot>& out,
                bool* refuted) {
    const bool array = b::with_block(tag, B.base, off, B.size, B.version, false, [](const auto& h) {
        return requires { h.entries_begin(); h.stride(); };
    });
    if (array) {
        if (!in_extent(off, p::ArrayHeader::HEADER_SIZE, B.size))
            return false;
        const std::optional<ArrayGeometry> g  = derive(B, off, tag);
        const ArrayGeometry                st = stamped(B, off);
        if (refuted)
            *refuted = !g || *g != st;
        // No further than both the stamp and the bytes allow: the entries the
        // stamp omits are the question a cycle answers. A geometry the bytes do
        // not settle is read no further than the next block.
        ArrayGeometry read = g ? ArrayGeometry{g->stride, std::min(g->count, st.count)} : st;
        if (!g) {
            const uint64_t entries = off + array_header(B, off, tag);
            const uint64_t limit   = next_block_after(B, off);
            read.count = limit < entries || read.stride == 0
                             ? 0 : static_cast<uint32_t>(std::min<uint64_t>(read.count, (limit - entries) / read.stride));
        }
        slots_of(B.base, B.size, B.version, off, tag, out, &read);
        return true;
    }
    if (off != HEADER_PARENT && grows(tag) &&
        off + classify_block(B.base, B.size, B.version, off).size > next_block_after(B, off))
        return false;
    slots_of(B.base, B.size, B.version, off, tag, out);
    return true;
}

// Does the word name a block that vouches for itself and carries exactly `want`?
Judged settle(const Board& B, uint64_t t, k::RecoveryCodes want) {
    const auto a     = B.anchors.find(static_cast<Offset>(t));
    const bool holds = a != B.anchors.end() && !a->second.lost && a->second.tag == want;
    return {holds ? Reading::Intact : Reading::Dangling, static_cast<Offset>(t)};
}

// Whether [at, at + length) starts inside, or runs across, something else the
// board charges -- FastFHIR's "tiles", asked of a stream. A block never contains
// another's first byte, and neither does a stream; its own charge, at `at`, is
// no evidence either way.
bool overlaps(const Board& B, uint64_t at, uint64_t length) {
    const auto next = B.cover.upper_bound(static_cast<Offset>(at));
    if (next != B.cover.end() && next->first < at + length)
        return true;
    auto before = B.cover.lower_bound(static_cast<Offset>(at));
    if (before == B.cover.begin())
        return false;
    --before;
    return before->first + before->second > at;
}

// Does a hole exactly a frame wide end at `stream`? The scan charges a frame
// only when it holds, so a frame that took the damage leaves that run behind,
// where an unframed stream abuts whatever precedes it.
bool lost_frame(const Board& B, uint64_t stream) {
    return std::any_of(B.holes.begin(), B.holes.end(), [stream](const Gap& h) {
        return static_cast<uint64_t>(h.start) + h.length == stream && h.length == b::TILE_PIXEL_DATA::header_size;
    });
}

// Whether something the board charges ends exactly at `at`.
bool meets_before(const Board& B, uint64_t at) {
    auto before = B.cover.lower_bound(static_cast<Offset>(at));
    if (before == B.cover.begin())
        return false;
    --before;
    return before->first + before->second == at;
}

// Whether something the board charges starts exactly at `end`, or the file ends there.
bool meets_after(const Board& B, uint64_t end) {
    return end == B.size || B.cover.contains(static_cast<Offset>(end));
}

// Whether [at, at + length) meets either neighbour. A stream the writer laid
// down meets both; one flipped word moves a stream off both, where damage to a
// neighbour can take away only one.
bool abuts(const Board& B, uint64_t at, uint64_t length) {
    return meets_before(B, at) || meets_after(B, at + length);
}

bool meets_both(const Board& B, uint64_t at, uint64_t length) {
    return meets_before(B, at) && meets_after(B, at + length);
}

// A tile entry names a stream, which carries no header. Its witnesses are its
// place in the file -- it starts inside nothing, runs across nothing, and never
// where a block vouches for itself -- and
// its frame, where it has one (CLAUDE.md, "TILE_PIXEL_DATA grows backwards"): a
// frame that holds must name this entry's tile, and one that took a flip leaves
// a hole where it stood. A stream that meets neither neighbour is not where its
// writer put it. With no frame and a clean place, the entry is believed. Its
// SIZE has no second witness.
Judged judge_tile(const Board& B, const Recovery::Slot& s) {
    if (s.stored == k::NULL_TILE)
        return {};
    const b::FILE_HEADER header = versioned_root(B.base, B.size);
    if (s.stored < header.extent() || !in_extent(s.stored, s.claim, B.size) ||
        B.anchors.contains(static_cast<Offset>(s.stored)) || overlaps(B, s.stored, s.claim))
        return {Reading::Dangling, static_cast<Offset>(s.stored)};
    const b::TILE_PIXEL_DATA frame{B.base, static_cast<Offset>(s.stored), B.size, B.version};
    if (frame.fits(b::TILE_PIXEL_DATA::header_size) &&
        frame.validation() == s.stored + b::TILE_PIXEL_DATA::offset::VALIDATION)
        return {frame.tile_index() == s.index ? Reading::Intact : Reading::Dangling,
                static_cast<Offset>(s.stored)};
    const bool placed = !lost_frame(B, s.stored) && abuts(B, s.stored, s.claim);
    return {placed ? Reading::Intact : Reading::Dangling, static_cast<Offset>(s.stored)};
}

// What a slot's bytes say on their own.
Judged judge(const Board& B, const Recovery::Slot& s) {
    switch (s.repr) {
        case Recovery::SlotRepr::Absolute:
        case Recovery::SlotRepr::Nested:
            if (s.stored == k::NULL_OFFSET)
                return {s.nullable ? Reading::Absent : Reading::Dangling, k::NULL_OFFSET};
            return settle(B, s.stored, s.expect);
        case Recovery::SlotRepr::TileEntry:
            return judge_tile(B, s);
    }
    return {};
}

// Each parent's decided children, in the order of its slots.
std::unordered_map<Offset, std::vector<Offset>> children(const Board& B) {
    std::vector<std::pair<const Recovery::Slot*, Offset>> links;
    links.reserve(B.owner.size());
    for (const auto& [child, slot] : B.owner)
        links.emplace_back(&slot, child);
    std::sort(links.begin(), links.end(), [](const auto& a, const auto& b) { return a.first->seat < b.first->seat; });
    std::unordered_map<Offset, std::vector<Offset>> kids;
    for (const auto& [slot, child] : links)
        kids[slot->parent].push_back(child);
    return kids;
}

// Every block the FILE_HEADER reaches through decided links, the header first,
// then depth first in the order of each parent's slots.
std::vector<Offset> attached(const Board& B, const std::unordered_map<Offset, std::vector<Offset>>& kids) {
    std::vector<Offset> members;
    if (!has_magic(B.base, B.size))
        return members;
    std::unordered_set<Offset> seen;
    std::vector<Offset> stack{HEADER_PARENT};
    while (!stack.empty()) {
        const Offset o = stack.back();
        stack.pop_back();
        if (!seen.insert(o).second)
            continue;
        members.push_back(o);
        if (const auto k = kids.find(o); k != kids.end())
            stack.insert(stack.end(), k->second.rbegin(), k->second.rend());
    }
    return members;
}

// Open a question, unless the same one is already open. Returns whether it did.
bool ask(Board& B, const Recovery::Point& pt) {
    const bool open = std::any_of(B.questions.begin(), B.questions.end(), [&pt](const Question& q) {
        return q.open && q.point.kind == pt.kind &&
               (pt.kind == Recovery::PointKind::Open ? q.point.slot.seat == pt.slot.seat
                                                     : q.point.array == pt.array);
    });
    if (!open)
        B.questions.push_back({pt, true});
    return !open;
}

}  // namespace

// =====================================================================
// THE CENSUS AS A PRODUCER — census()
// =====================================================================

Recovery::Census Recovery::census() const {
    const Board B    = take_census(m_base, m_size, scan());
    const auto  kids = children(B);
    Census c;
    c.extent  = m_size;
    c.anchors = static_cast<std::size_t>(
        std::count_if(B.anchors.begin(), B.anchors.end(), [](const auto& a) { return !a.second.lost; }));
    for (const auto& [child, slot] : B.owner)
        c.edges.push_back({slot, child});
    std::sort(c.edges.begin(), c.edges.end(), [](const Edge& a, const Edge& b) { return a.slot.seat < b.slot.seat; });

    // The header's island, then one per self-validating block no decided link names.
    std::unordered_set<Offset> placed;
    const auto grow = [&](Offset root, bool attached_to_header) {
        Island island{root, attached_to_header, {}};
        std::vector<Offset> stack{root};
        while (!stack.empty()) {
            const Offset o = stack.back();
            stack.pop_back();
            if (!placed.insert(o).second)
                continue;
            island.members.push_back(o);
            if (const auto k = kids.find(o); k != kids.end())
                stack.insert(stack.end(), k->second.rbegin(), k->second.rend());
        }
        c.islands.push_back(std::move(island));
    };
    if (has_magic(m_base, m_size))
        grow(HEADER_PARENT, true);
    for (const auto& [off, a] : B.anchors)
        if (!a.lost && !B.owner.contains(off) && !placed.contains(off))
            grow(off, false);
    c.holes = B.holes;
    for (const Question& q : B.questions)
        c.points.push_back(q.point);
    const auto key = [](const Point& pt) {
        return pt.kind == PointKind::Open ? pt.slot.seat : pt.array + p::ArrayHeader::STRIDE;
    };
    std::sort(c.points.begin(), c.points.end(), [&key](const Point& a, const Point& b) { return key(a) < key(b); });
    return c;
}

// =====================================================================
// THE BYTE CENSUS — scan() and find_gaps()
// =====================================================================

// Every position whose eight bytes hold its own offset. A random word does so
// with probability 2^-64, so this alone finds every block whose self-offset
// survived. Split across workers when the file is large enough to pay for
// them; chunks overlap by a header's width so a block on a boundary is seen.
//
// A tile stream has no self-offset, so it is charged to the self-validating
// tile offsets entry that names it, with its frame where one holds. The frame
// SEARCH -- frames of streams whose entries are lost -- is the solver's
// (MIGRATION.md RB-6): a bare 40-bit self-offset is too weak to scan for.
FileMap Recovery::scan() const {
    FileMap map;
    map.file_size = m_size;
    const b::FILE_HEADER header = versioned_root(m_base, m_size);
    if (has_magic(m_base, m_size))
        map[0] = {Abstraction::MAP_ENTRY_FILE_HEADER, 0, header.extent()};

    const unsigned hw      = std::thread::hardware_concurrency();
    const size_t   workers = (m_size >= (Size{1} << 20) && hw > 1) ? std::min<size_t>(hw, 8u) : 1;
    const size_t   chunk   = (m_size + workers - 1) / workers;
    std::vector<std::vector<Offset>> found(workers);
    std::vector<std::exception_ptr>  errors(workers);
    const auto sweep = [&](size_t w) {
        try {
            const size_t end = std::min<size_t>(m_size, (w + 1) * chunk +
                                                    (w + 1 < workers ? p::BlockHeader::HEADER_SIZE - 1 : 0));
            for (size_t off = w * chunk; off + p::BlockHeader::HEADER_SIZE <= end; ++off)
                if (load<std::uint64_t>(m_base + off) == off)
                    found[w].push_back(static_cast<Offset>(off));
        } catch (const std::exception&) {
            errors[w] = std::current_exception();  // rethrown on the calling thread below
        }
    };
    std::vector<std::thread> threads;
    for (size_t w = 1; w < workers; ++w)
        threads.emplace_back(sweep, w);
    sweep(0);
    for (std::thread& t : threads)
        t.join();
    for (const std::exception_ptr& e : errors)
        if (e)
            std::rethrow_exception(e);

    // IFE records only a block whose tag it can size: an unknown type has no
    // extent to tile with. The fact is kept on `failures`, never dropped.
    for (const std::vector<Offset>& offsets : found)
        for (const Offset off : offsets) {
            if (map.contains(off))
                continue;  // the header, or a chunk overlap
            const k::RecoveryCodes tag = tag_at(m_base, m_size, off);
            if (!sizable_tag(tag)) {
                map.failures.push_back({Abstraction::ProducerFailureKind::ScanTagInvalid, off,
                                        Abstraction::MAP_ENTRY_UNDEFINED, tag,
                                        "the self-offset holds, but the tag is no type this build knows"});
                continue;
            }
            map[off] = classify_block(m_base, m_size, header.__version, off);
        }

    // The streams are charged through the census's own reading of each tile
    // offsets array -- under the geometry the bytes support, so a flipped
    // STRIDE or COUNT charges the streams the entries really name.
    //
    // What the scan is certain of is the header, which the magic vouches for,
    // and each block's signature, which its self-offset does. A block's extent
    // is a claim its tag and lengths make, and the census weighs those.
    Board B{.base = m_base, .size = m_size, .version = header.__version,
            .newer_file = file_is_newer(m_base, m_size)};
    for (const auto& [off, e] : map) {
        B.cover.emplace(off, off == 0 ? e.size : Size{p::BlockHeader::HEADER_SIZE});
        if (off != 0)
            B.anchors.emplace(off, Anchor{Abstraction::recovery_for(e.type), false, true});
    }
    std::vector<Slot> slots;
    for (const auto& [off, a] : B.anchors)
        if (a.tag == k::RecoveryCodes::RECOVER_TILE_OFFSETS)
            read_slots(B, off, a.tag, slots, nullptr);
    // A stream that would start inside, or run across, what the scan is certain
    // of is not charged: a flipped entry must not take the bytes of the header
    // or of a block's signature. Streams that overlap each other, or a block's
    // claimed extent, are all charged, and the census weighs them.
    const auto charge = [&](Offset at, Size length, Abstraction::MapEntryType type) {
        if (overlaps(B, at, length) || B.cover.contains(at) || map.contains(at))
            return false;
        map[at] = {type, at, length};
        return true;
    };
    {
        for (const Slot& s : slots) {
            if (s.stored == k::NULL_TILE || s.claim == 0 || !in_extent(s.stored, s.claim, m_size) ||
                !charge(static_cast<Offset>(s.stored), s.claim, Abstraction::MAP_ENTRY_TILE_PIXEL_DATA))
                continue;
            const b::TILE_PIXEL_DATA frame{m_base, static_cast<Offset>(s.stored), m_size, header.__version};
            if (frame.fits(b::TILE_PIXEL_DATA::header_size) &&
                frame.validation() == s.stored + b::TILE_PIXEL_DATA::offset::VALIDATION &&
                frame.tile_index() == s.index)
                charge(static_cast<Offset>(s.stored - b::TILE_PIXEL_DATA::header_size),
                       b::TILE_PIXEL_DATA::header_size, Abstraction::MAP_ENTRY_TILE_FRAME);
        }
    }
    find_gaps(map);
    return map;
}

// Tile the file and classify every run of bytes no entry covers (REC-18). A
// run after the last entry is slack. A run is benign version skew only when the
// file is newer than this reader and it trails a block of a type that grows by
// appending fields -- a longer block, not damage. FastFHIR also demands that
// every block of the tag trail the same run, which needs two of them; IFE's
// growing blocks are mostly single (FILE_HEADER, TILE_TABLE, METADATA), so a
// single block's run qualifies alone, and a repeated type still must agree.
// Otherwise the run is a hole.
void Recovery::find_gaps(FileMap& map) const {
    map.gaps.clear();
    if (map.empty())
        return;

    // Each entry covers its extent, but never past the next entry: a block
    // never contains another block's first byte, and a stamped count or length
    // that damage inflated would otherwise swallow every hole behind it.
    struct Run { Offset start; Size length; k::RecoveryCodes after; };
    std::vector<Run> runs;
    std::unordered_map<uint16_t, size_t> instances;
    uint64_t         cursor = 0;
    k::RecoveryCodes after  = k::RecoveryCodes::RECOVER_UNDEFINED;
    for (auto it = map.begin(); it != map.end(); ++it) {
        const auto& [off, e] = *it;
        if (off > cursor)
            runs.push_back({static_cast<Offset>(cursor), static_cast<Size>(off - cursor), after});
        const auto     next = std::next(it);
        const uint64_t end  = static_cast<uint64_t>(off) + e.size;
        cursor = std::max<uint64_t>(cursor, next == map.end() ? end : std::min<uint64_t>(end, next->first));
        after  = Abstraction::recovery_for(e.type);
        ++instances[static_cast<uint16_t>(after)];
    }
    if (map.file_size > cursor)
        runs.push_back({static_cast<Offset>(cursor), static_cast<Size>(map.file_size - cursor), after});

    const bool newer = file_is_newer(m_base, m_size);
    std::map<std::pair<uint16_t, Size>, size_t> trails;  // (tag, run length) -> how many
    for (const Run& r : runs)
        ++trails[{static_cast<uint16_t>(r.after), r.length}];
    for (const Run& r : runs) {
        const size_t same = trails[{static_cast<uint16_t>(r.after), r.length}];
        Gap g{r.start, r.length, r.after, GapClass::Hole, "unattributed bytes"};
        if (static_cast<uint64_t>(r.start) + r.length >= map.file_size) {
            g.class_ = GapClass::Trailing;
            g.why    = "slack past the last entry";
        } else if (newer && r.after != k::RecoveryCodes::RECOVER_UNDEFINED && grows(r.after) &&
                   same == instances[static_cast<uint16_t>(r.after)]) {
            g.class_ = GapClass::VersionSkew;
            g.why    = "every block of this tag trails the same run, and the file is newer";
        } else if (r.length < p::BlockHeader::HEADER_SIZE) {
            g.why = "unattributed, but too small to have held a block header";
        }
        map.gaps.push_back(g);
    }
}

// =====================================================================
// LEAF HELPERS
// =====================================================================

// The extent is the mapping's. Deciding it from the header's FILE_SIZE, the
// one fact a damaged header can misstate, is RB-4's.
Recovery::Recovery(const FileAccessInfo& info) noexcept
    : m_base(info.file_ptr), m_size(info.file_size) {}

}  // namespace Iris::File
