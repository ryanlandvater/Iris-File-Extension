/**
 * @file IFE_Recovery.cpp
 * @brief Recovery implementation — MIGRATION.md RC-1…RC-6. The FastFHIR
 *        REC-10…18 port (RC-1…RC-5) plus the REC-19 rewrite's lessons
 *        (RC-6): one-surviving-witness walk descent, a typed failure audit
 *        on the report, a coherence-gated reapply of repaired blocks, a
 *        parallel scan census, and sized-evidence hole ranking.
 *
 * Every parent→child reference is encoded twice: the parent's slot {expected
 * type, stored offset} and the child's block header {VALIDATION == own
 * offset, RECOVERY tag}. recover() reconciles each edge by whichever witness
 * survives, classifies the repair, and reports it — never silently; apply()
 * is the only path that mutates.
 *
 * The byte-level machinery is the primitives' (IFE_Primitives.hpp); the
 * spec-derived facts — per-block extents, the tag→map-entry vocabulary — are
 * the generated layer's. This file is the reconciliation on top of both.
 *
 * File order (REC-19.1): leaf helpers first, then the one-level enumerators
 * and the walk driver, then the producers, then the entrance recover() —
 * everything it calls is visible above it, so the call stack reads
 * top→bottom; apply() and the write helpers it needs are last.
 */

#include "IFE_Recovery.hpp"
#include "IFE_Primitives.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Iris::File {

namespace k  = ::Iris::File::constants;
namespace b  = ::Iris::File::blocks;
namespace p  = ::Iris::File::primitives;

namespace {

// ---------------------------------------------------------------------------
// Leaf helpers — bounds-checked wire reads. Nothing here dereferences a
// header field at construction; every read is guarded by the size argument.
// ---------------------------------------------------------------------------

/// Can `__n` bytes be read at `__off` in a buffer of `__size`?
///
/// THE SUBTRACTION IS THE POINT. Every offset here comes off the wire, so the
/// natural `__off + __n <= __size` is a bug: a corrupted slot near 2^64 wraps
/// past the guard and the load then runs backward off the front of the
/// mapping. That is not a theoretical value — NULL_OFFSET is all-ones and is
/// the most common thing a nullable slot holds (CIPHER_OFFSET in every fixture
/// to date), so ONE flipped bit in an unused slot produces 0xFF…FE, and
/// 0xFF…FE + 10 wraps to 8. Caught by an exhaustive single-byte damage sweep
/// under ASan (2026-08-28); the additive form must not come back. This is the
/// same shape as p::BlockHeader::fits, minus its non-zero-offset rule (offset
/// zero is the root, and legal to read).
[[nodiscard]] constexpr bool in_range(uint64_t __off, uint64_t __n, uint64_t __size) noexcept {
    return __off <= __size && __size - __off >= __n;
}

/// The recovery tag read at `at + 8` (the universal header's RECOVERY slot),
/// or RECOVER_UNDEFINED when the block header cannot fit.
k::RecoveryCodes tag_at(const BYTE* __base, Size __size, Offset __at) noexcept {
    if (!in_range(__at, p::BlockHeader::HEADER_SIZE, __size))
        return k::RecoveryCodes::RECOVER_UNDEFINED;
    return static_cast<k::RecoveryCodes>(
        ::Iris::File::load<std::uint16_t>(__base + __at + p::BlockHeader::RECOVERY));
}

/// The raw u64 stored in a parent slot. NULL_OFFSET means the slot is absent
/// — a legal empty edge, not damage — and is never enumerated.
Offset slot_value(const BYTE* __base, Size __size, Offset __parent, Size __slot) noexcept {
    if (!in_range(__parent, __slot, __size) || !in_range(__parent + __slot, 8, __size))
        return k::NULL_OFFSET;
    return static_cast<Offset>(::Iris::File::load<std::uint64_t>(__base + __parent + __slot));
}

/// Witness 2a/2b for a normal block: VALIDATION and RECOVERY read at target.
/// A target that does not admit a whole block header leaves both witnesses at
/// their absent defaults — the edge is then classified on the parent alone.
void read_witnesses(const BYTE* __base, Size __size, Abstraction::BlockRef& __ref) noexcept {
    if (in_range(__ref.target, p::BlockHeader::HEADER_SIZE, __size)) {
        __ref.validation = static_cast<Offset>(::Iris::File::load<std::uint64_t>(__base + __ref.target));
        __ref.recovery    = tag_at(__base, __size, __ref.target);
    }
}

/// Record one parent→child reference from a plain u64 slot. Absent slots
/// (NULL_OFFSET) are not edges and are never enumerated.
void slot_ref(std::vector<Abstraction::BlockRef>& __refs, const BYTE* __base, Size __size,
              Offset __parent, Size __slot, Abstraction::MapEntryType __expected) {
    Abstraction::BlockRef ref;
    ref.parent   = __parent;
    ref.slot     = __slot;
    ref.expected = __expected;
    ref.target   = slot_value(__base, __size, __parent, __slot);
    if (ref.target == k::NULL_OFFSET) return;
    read_witnesses(__base, __size, ref);
    __refs.push_back(std::move(ref));
}

// ---------------------------------------------------------------------------
// RC-6 — the REC-19 admission and audit helpers. One rule set, shared by the
// walk driver, the orphan pass, the reapply and the classifier.
// ---------------------------------------------------------------------------

/// One-surviving-witness descent (FastFHIR defect 1): a child is walkable
/// when its self-offset is its own address, or within the flip budget of it.
/// "The tag corroborates but the self-offset does not" describes two very
/// different situations that must not be treated alike — a real child whose
/// VALIDATION took the flip (the stored word is a Hamming neighbour of the
/// address), or a parent offset flipped onto arbitrary bytes whose tag
/// happens to match (the stored word is unrelated, so the distance is
/// large). The budget is the discriminator; the same D1/D2 discipline the
/// classifier applies, so the walk and the verdict cannot disagree about
/// what is repairable.
[[nodiscard]] bool self_repairable(const BYTE* __base, Size __size, Offset __target) noexcept {
    if (!in_range(__target, p::BlockHeader::HEADER_SIZE, __size)) return false;
    const Offset validation = static_cast<Offset>(::Iris::File::load<std::uint64_t>(__base + __target));
    if (validation == __target) return true;
    return Recovery::hamming_cost(static_cast<std::uint64_t>(validation),
                                  static_cast<std::uint64_t>(__target))
           <= IFE_RECOVERY_MAX_FLIPS;
}

/// Wrong turn 1, kept: the wire tag corroborates the slot's compiled
/// expectation when it maps to it — never the reverse. A tag-damaged child
/// is still walked, under the slot's declared type (the parent is the
/// surviving witness); this predicate is what the reapply's coherence gate
/// and the classifier use to tell "the tag was flipped" from "the address
/// was flipped onto a different block".
[[nodiscard]] bool tag_corroborates(const BYTE* __base, Size __size, Offset __target,
                                    Abstraction::MapEntryType __expected) noexcept {
    if (!in_range(__target, p::BlockHeader::HEADER_SIZE, __size)) return false;
    const k::RecoveryCodes tag = tag_at(__base, __size, __target);
    return tag != k::RecoveryCodes::RECOVER_UNDEFINED
        && Abstraction::entry_for(tag) == __expected;
}

/// REC-19.3 — the one routine every reference is judged by. Returns true
/// when the reference is damaged, and records a typed failure (when
/// `__failures` is non-null): a child that does not self-validate is
/// InvalidSelfRef; a child that validates but carries a tag contradicting
/// the slot's expectation is VTableRecoveryMismatch. Recording is NOT a
/// descent gate — the walk descends on self_repairable regardless — it is
/// what makes a lost subtree VISIBLE in the report: before RC-6, one flipped
/// bit in a child's VALIDATION cost every reference below it while the
/// report showed zero failures, because the references were never
/// enumerated to be repaired.
///
/// Takes no buffer: a BlockRef already carries BOTH witnesses, read once when
/// the reference was enumerated. Re-reading the bytes here would let the audit
/// and the enumeration disagree about the same reference.
[[nodiscard]] bool audit_ref(Size __size, const Abstraction::BlockRef& __r,
                             std::vector<Abstraction::ProducerFailure>* __failures) noexcept {
    if (__r.target == k::NULL_OFFSET) return false;                            // absent — not a reference
    if (__r.expected == Abstraction::MAP_ENTRY_TILE_PIXEL_DATA) return false;  // no header to judge
    if (!in_range(__r.target, p::BlockHeader::HEADER_SIZE, __size)) return false;  // out of arena — classified, not audited
    if (__r.validation == __r.target) {
        if (__r.recovery != k::RecoveryCodes::RECOVER_UNDEFINED
            && Abstraction::entry_for(__r.recovery) == __r.expected)
            return false;  // both witnesses agree — coherent
        if (__failures)
            __failures->push_back({Abstraction::ProducerFailureKind::VTableRecoveryMismatch,
                                   __r.target, __r.expected, __r.recovery,
                                   "wire tag does not match the slot's expected recovery"});
        return true;
    }
    if (__failures)
        __failures->push_back({Abstraction::ProducerFailureKind::InvalidSelfRef,
                               __r.target, __r.expected, __r.recovery,
                               "slot names a block whose VALIDATION is not its own address"});
    return true;
}

/// The address a repaired reference should be FOLLOWED to (RC-6, FastFHIR
/// repaired_target). Not always the one the slot names: Corroborated and
/// HoleCorroborated mean the REPOINT hypothesis won — the ranker decided the
/// parent's stored offset is the damaged half and the real child is the
/// candidate it found — so follow the candidate, not the rejected offset.
/// TagRepaired and PositionRepaired are in-place repairs and keep their
/// address. Following the rejected offset under the declared type is what
/// invented references in FastFHIR's 5-of-40 single-bit trials, every one a
/// flip in the PARENT's slot.
Offset repaired_target(const Abstraction::BlockVerdict& __v) noexcept {
    if ((__v.class_ == Abstraction::RepairClass::Corroborated ||
         __v.class_ == Abstraction::RepairClass::HoleCorroborated) &&
        __v.repaired != k::NULL_OFFSET)
        return __v.repaired;
    return __v.block.target;
}

/// The walker context: base/size plus the file's declared version, which the
/// version-gated accessors and versioned extents need.
struct Walker {
    const BYTE* base    = nullptr;
    Size        size    = 0;
    uint32_t    version = 0;
};

// ---------------------------------------------------------------------------
// One-level enumerators — the 18-edge inventory (MIGRATION.md RC-1.1). Each
// enumerates ONLY the block's own slots; descent is the walk driver's job
// (RC-6 / REC-19.4), so the same enumerators serve the walk, the orphan pass
// and the reapply — one mechanism, three call sites.
// ---------------------------------------------------------------------------

void enumerate_tile_offsets(Walker& w, const b::TILE_OFFSETS& offsets,
                            std::vector<Abstraction::BlockRef>& out) {
    // The one edge whose child has no header: a tile stream is unframed, so
    // witness 1b is the entry's OFFSET and witness 2 is the optional frame —
    // a u40 self-offset five bytes before the stream, plus TILE_INDEX. The
    // entry's SIZE is the extent, never carried by the frame.
    //
    // The element range is bounded by the arena, never by the stamped COUNT
    // alone: the walk now descends into repairable arrays whose COUNT may be
    // the damaged half (the pre-RC-6 walk was protected by validate()).
    // entry(i) performs no bounds check, so a garbage COUNT must not drive
    // the reads past the mapping.
    // Bounded by readable_entries (IFE_Primitives.hpp): whole entries inside
    // the file, by geometry. `(size - begin) / stride` was not enough — a
    // damaged stride narrower than the entry carried the last entry's fields
    // past EOF (ASan, ife_recovery_bench_tests, 2026-09-28).
    for (uint32_t i = 0, cap = readable_entries(offsets); i < cap; ++i) {
        const auto entry = offsets.entry(i);
        const Offset stream = static_cast<Offset>(entry.offset());
        if (stream == k::NULL_TILE || stream == k::NULL_OFFSET) continue;
        Abstraction::BlockRef ref;
        ref.parent   = offsets.__offset;
        ref.slot     = (entry.__offset - offsets.__offset)
                     + b::TILE_OFFSETS::TILE_OFFSET::offset::OFFSET;
        ref.expected = Abstraction::MAP_ENTRY_TILE_PIXEL_DATA;
        ref.target   = stream;
        ref.extent   = entry.size_field();
        // signature_matches only guarantees the five VALIDATION bytes are in
        // the file; TILE_INDEX sits four bytes BEHIND those, so an anchor at
        // 5..8 with a self-consistent u40 would read before the mapping. The
        // frame grows backward, so every read behind the anchor owes its own
        // bound (see the layout note in CLAUDE.md).
        constexpr Size FRAME_BACK = p::FrameHeader::VALIDATION_SIZE
                                  + b::TILE_PIXEL_DATA::size::TILE_INDEX;
        if (stream >= FRAME_BACK && p::FrameHeader::signature_matches(w.base, stream, w.size)) {
            ref.validation = static_cast<Offset>(p::FrameHeader::validation_at(w.base, stream));
            // NOTE: RecoveryCodes is a u16, so this narrows the u32 TILE_INDEX.
            // Nothing consumes this copy — the tile adjudication in classify()
            // reads the full value from the bytes — but it is a lossy field
            // wearing an exact field's name.
            ref.recovery   = static_cast<k::RecoveryCodes>(
                ::Iris::File::load<std::uint32_t>(w.base + stream - FRAME_BACK));
        }
        out.push_back(std::move(ref));
    }
}

void enumerate_attributes(Walker& w, const b::ATTRIBUTES& attrs,
                          std::vector<Abstraction::BlockRef>& out) {
    const Offset parent = attrs.__offset;
    slot_ref(out, w.base, w.size, parent,
             b::ATTRIBUTES::offset::SIZES_OFFSET, Abstraction::MAP_ENTRY_ATTRIBUTE_SIZES);
    slot_ref(out, w.base, w.size, parent,
             b::ATTRIBUTES::offset::BYTES_OFFSET, Abstraction::MAP_ENTRY_ATTRIBUTE_BYTES);

    // Nested values: a nested ATTRIBUTE_SIZE entry names a slice of the byte
    // run whose items are absolute offsets to child ATTRIBUTES blocks. The
    // slot address of such an edge is inside the byte run, not the parent
    // header — the one place a parent edge does not sit at an offset::SLOT.
    // The old gate demanded sizes.validate() && bytes.validate() here, so one
    // flipped bit in either array's VALIDATION dropped the nested refs even
    // after the walk had recovered the arrays themselves (REC-19 defect 3:
    // array elements must survive a damaged array header). The accessors'
    // fits-check is the only gate left; the cursor overruns below bound the
    // walk against garbage sizes.
    const auto sizes = attrs.sizes_offset();
    const auto bytes = attrs.bytes_offset();
    if (!sizes.in_bounds() || !bytes.in_bounds()) return;
    // The packed run is [key][value] per size entry, in order — the same
    // slicing slice_attributes() performs (IFE_Runtime.cpp §Attribute
    // slicing). A cursor that advanced by value_size alone would read nested
    // offsets from key text.
    //
    // TWO ARENA BOUNDS, for the same reason as the other array walkers: a
    // damaged COUNT must not drive entry reads past the mapping, and the
    // byte run's own COUNT is equally untrusted — the blob is clamped to the
    // bytes the file actually has before a single nested offset is read.
    // Bounded by readable_entries (IFE_Primitives.hpp): whole entries inside
    // the file, by geometry. `(size - begin) / stride` was not enough — a
    // damaged stride narrower than the entry carried the last entry's fields
    // past EOF (ASan, ife_recovery_bench_tests, 2026-09-28).
    const uint32_t s_cap = readable_entries(sizes);
    const ::Iris::File::ByteSpan blob_raw = bytes.bytes();
    const Size blob_off = (blob_raw.data >= w.base)
                              ? static_cast<Size>(blob_raw.data - w.base) : w.size;
    const Size blob_size = (blob_off <= w.size)
                               ? std::min(blob_raw.size, w.size - blob_off) : 0;
    const ::Iris::File::ByteSpan blob{blob_raw.data, blob_size};
    Size cursor = 0;
    for (uint32_t i = 0; i < s_cap; ++i) {
        const auto entry = sizes.entry(i);
        const Size key_size   = entry.key_size();
        const Size value_size = entry.value_size();
        if (key_size > blob.size - cursor) return;  // overrun: stop the walk
        cursor += key_size;
        if (value_size > blob.size - cursor) return;
        if (entry.kind() == k::AttributeKinds::ATTRIBUTE_NESTED) {
            for (Size j = 0; j + b::NESTED_OFFSET_SIZE <= value_size; j += b::NESTED_OFFSET_SIZE) {
                const Offset child = static_cast<Offset>(
                    ::Iris::File::load<std::uint64_t>(blob.data + cursor + j));
                if (child == k::NULL_OFFSET) continue;
                Abstraction::BlockRef ref;
                ref.parent   = parent;
                ref.slot     = (bytes.__offset - attrs.__offset)
                             + b::ATTRIBUTE_BYTES::header_size + cursor + j;
                ref.expected = Abstraction::MAP_ENTRY_ATTRIBUTES;
                ref.target   = child;
                read_witnesses(w.base, w.size, ref);
                out.push_back(std::move(ref));
                // No descent here — the driver queues (child, ATTRIBUTES).
            }
        }
        cursor += value_size;
    }
}

void enumerate_images(Walker& w, const b::IMAGES& images,
                      std::vector<Abstraction::BlockRef>& out) {
    // Bounded by readable_entries (IFE_Primitives.hpp): whole entries inside
    // the file, by geometry. `(size - begin) / stride` was not enough — a
    // damaged stride narrower than the entry carried the last entry's fields
    // past EOF (ASan, ife_recovery_bench_tests, 2026-09-28).
    for (uint32_t i = 0, cap = readable_entries(images); i < cap; ++i) {
        const auto entry = images.entry(i);
        const auto bytes = entry.bytes_offset();
        if (bytes.__offset == k::NULL_OFFSET) continue;
        Abstraction::BlockRef ref;
        ref.parent   = images.__offset;
        ref.slot     = (entry.__offset - images.__offset)
                     + b::IMAGES::IMAGE_ENTRY::offset::BYTES_OFFSET;
        ref.expected = Abstraction::MAP_ENTRY_IMAGE_BYTES;
        ref.target   = bytes.__offset;
        read_witnesses(w.base, w.size, ref);
        out.push_back(std::move(ref));
    }
}

void enumerate_annotations(Walker& w, const b::ANNOTATIONS& annotations,
                           std::vector<Abstraction::BlockRef>& out) {
    const Offset parent = annotations.__offset;
    slot_ref(out, w.base, w.size, parent,
             b::ANNOTATIONS::offset::GROUP_SIZES_OFFSET, Abstraction::MAP_ENTRY_ANNOTATION_GROUP_SIZES);
    slot_ref(out, w.base, w.size, parent,
             b::ANNOTATIONS::offset::GROUP_BYTES_OFFSET, Abstraction::MAP_ENTRY_ANNOTATION_GROUP_BYTES);
    // Bounded by readable_entries (IFE_Primitives.hpp): whole entries inside
    // the file, by geometry. `(size - begin) / stride` was not enough — a
    // damaged stride narrower than the entry carried the last entry's fields
    // past EOF (ASan, ife_recovery_bench_tests, 2026-09-28).
    for (uint32_t i = 0, cap = readable_entries(annotations); i < cap; ++i) {
        const auto entry = annotations.entry(i);
        const auto bytes = entry.bytes_offset();
        if (bytes.__offset == k::NULL_OFFSET) continue;
        Abstraction::BlockRef ref;
        ref.parent   = parent;
        ref.slot     = (entry.__offset - annotations.__offset)
                     + b::ANNOTATIONS::ANNOTATION_ENTRY::offset::BYTES_OFFSET;
        ref.expected = Abstraction::MAP_ENTRY_ANNOTATION_BYTES;
        ref.target   = bytes.__offset;
        read_witnesses(w.base, w.size, ref);
        out.push_back(std::move(ref));
    }
}

void enumerate_metadata(Walker& w, const b::METADATA& metadata,
                        std::vector<Abstraction::BlockRef>& out) {
    const Offset parent = metadata.__offset;
    slot_ref(out, w.base, w.size, parent,
             b::METADATA::offset::ATTRIBUTES_OFFSET, Abstraction::MAP_ENTRY_ATTRIBUTES);
    slot_ref(out, w.base, w.size, parent,
             b::METADATA::offset::IMAGES_OFFSET, Abstraction::MAP_ENTRY_IMAGES);
    slot_ref(out, w.base, w.size, parent,
             b::METADATA::offset::ICC_COLOR_OFFSET, Abstraction::MAP_ENTRY_ICC_PROFILE);
    slot_ref(out, w.base, w.size, parent,
             b::METADATA::offset::ANNOTATIONS_OFFSET, Abstraction::MAP_ENTRY_ANNOTATIONS);
    slot_ref(out, w.base, w.size, parent,
             b::METADATA::offset::CLINICAL_OFFSET, Abstraction::MAP_ENTRY_CLINICAL_METADATA);
}

void enumerate_tile_table(Walker& w, const b::TILE_TABLE& table,
                          std::vector<Abstraction::BlockRef>& out) {
    const Offset parent = table.__offset;
    slot_ref(out, w.base, w.size, parent,
             b::TILE_TABLE::offset::CIPHER_OFFSET, Abstraction::MAP_ENTRY_CIPHER);
    slot_ref(out, w.base, w.size, parent,
             b::TILE_TABLE::offset::LAYER_EXTENTS_OFFSET, Abstraction::MAP_ENTRY_LAYER_EXTENTS);
    slot_ref(out, w.base, w.size, parent,
             b::TILE_TABLE::offset::TILE_OFFSETS_OFFSET, Abstraction::MAP_ENTRY_TILE_OFFSETS);
}

/// Takes no handle, unlike its siblings: the root is at offset 0 BY
/// DEFINITION, so both slots are addressed absolutely. The caller still
/// builds the handle — that is its validity gate — and then has nothing to
/// pass.
void enumerate_file_header(Walker& w, std::vector<Abstraction::BlockRef>& out) {
    slot_ref(out, w.base, w.size, 0,
             b::FILE_HEADER::offset::TILE_TABLE_OFFSET, Abstraction::MAP_ENTRY_TILE_TABLE);
    slot_ref(out, w.base, w.size, 0,
             b::FILE_HEADER::offset::METADATA_OFFSET, Abstraction::MAP_ENTRY_METADATA);
}

/// The one-level enumeration under a declared (corroborated) type at an
/// offset. Used by the walk driver, the orphan pass and the reapply — one
/// mechanism, three call sites (RC-6 / REC-19.4/.7). Leaf types (byte runs,
/// streams, the frame, fixed blocks with no outgoing slots) enumerate
/// nothing.
///
/// Every handle test in this file is `in_bounds()`, never the bool. The bool
/// demands validate(), and this engine exists to read blocks that fail it: an
/// array whose COUNT overruns must still emit its references (bounded by
/// geometry below), and a child whose VALIDATION took the flip is the thing
/// being hunted. Testing the bool here silently stops recovery at the first
/// damaged block.
void enumerate_one_level(Offset __off, Abstraction::MapEntryType __type, Walker& w,
                         std::vector<Abstraction::BlockRef>& out) {
    if (!in_range(__off, p::BlockHeader::HEADER_SIZE, w.size)) return;
    switch (__type) {
        case Abstraction::MAP_ENTRY_FILE_HEADER: {
            if (__off == 0)
                if (const auto root = versioned_root(w.base, w.size); root.in_bounds())
                    enumerate_file_header(w, out);
            break;
        }
        case Abstraction::MAP_ENTRY_TILE_TABLE: {
            if (const b::TILE_TABLE t{w.base, __off, w.size, w.version}; t.in_bounds())
                enumerate_tile_table(w, t, out);
            break;
        }
        case Abstraction::MAP_ENTRY_TILE_OFFSETS: {
            if (const b::TILE_OFFSETS t{w.base, __off, w.size, w.version}; t.in_bounds())
                enumerate_tile_offsets(w, t, out);
            break;
        }
        case Abstraction::MAP_ENTRY_METADATA: {
            if (const b::METADATA t{w.base, __off, w.size, w.version}; t.in_bounds())
                enumerate_metadata(w, t, out);
            break;
        }
        case Abstraction::MAP_ENTRY_ATTRIBUTES: {
            if (const b::ATTRIBUTES t{w.base, __off, w.size, w.version}; t.in_bounds())
                enumerate_attributes(w, t, out);
            break;
        }
        case Abstraction::MAP_ENTRY_IMAGES: {
            if (const b::IMAGES t{w.base, __off, w.size, w.version}; t.in_bounds())
                enumerate_images(w, t, out);
            break;
        }
        case Abstraction::MAP_ENTRY_ANNOTATIONS: {
            if (const b::ANNOTATIONS t{w.base, __off, w.size, w.version}; t.in_bounds())
                enumerate_annotations(w, t, out);
            break;
        }
        default: break;  // leaves: CIPHER, LAYER_EXTENTS, byte runs, streams, frame
    }
}

// ---------------------------------------------------------------------------
// The walk driver (RC-6 / REC-19.4) — DFS through references, one-surviving-
// witness descent, per-reference audit. `visited` bounds total work (each
// block is enumerated at most once, so cycles and re-walks are impossible);
// `depth` mirrors FastFHIR's FF_RECOVERY_MAX_DEPTH discipline.
// ---------------------------------------------------------------------------

struct WalkStep {
    Offset       off;
    Abstraction::MapEntryType type;
    std::size_t  depth;
};

// Forward declarations (REC-19.1): free helpers defined below their callers,
// in call order — drive_walk admits parent-attested blocks into the census
// map and needs the extent model to size them.
::Iris::File::Size extent_of(const BYTE*, ::Iris::File::Size, ::Iris::File::Offset, std::uint32_t, Abstraction::MapEntryType) noexcept;
::Iris::File::Size extent_header_size(Abstraction::MapEntryType) noexcept;
::Iris::File::Size claimable(::Iris::File::Offset, ::Iris::File::Size, ::Iris::File::Size) noexcept;

bool drive_walk(const BYTE* __base, Size __size, uint32_t __version,
                std::unordered_set<Offset>& __visited,
                std::vector<Abstraction::BlockRef>& __refs,
                std::vector<Abstraction::ProducerFailure>* __failures,
                Abstraction::FileMap* __map,
                std::vector<std::pair<Offset, Abstraction::MapEntryType>> __seeds) noexcept {
    Walker w{__base, __size, __version};
    bool admitted = false;
    std::vector<WalkStep> stack;
    stack.reserve(__seeds.size());
    for (const auto& s : __seeds) stack.push_back({s.first, s.second, 0});
    while (!stack.empty()) {
        const WalkStep s = stack.back();
        stack.pop_back();
        if (!__visited.insert(s.off).second) continue;
        if (s.depth >= IFE_RECOVERY_MAX_DEPTH) continue;
        // PARENT-ATTESTED ADMISSION (REC-19 defect 2/5): a block the census
        // missed — its VALIDATION was the damaged half, so scan() could not
        // find it by signature — is still real: the parent names its address
        // and its type. Admit it to the map, sized by the same extent model
        // the census uses, so the gap sweep tiles its run instead of
        // reporting a hole for a block the walk just recovered. The block's
        // own tag must corroborate the slot's type — the strict rule below —
        // and its full header must fit, or extent_of would read past the
        // mapping.
        if (__map && !__map->count(s.off) && s.type != Abstraction::MAP_ENTRY_TILE_PIXEL_DATA &&
            in_range(s.off, extent_header_size(s.type), __size)) {
            note(*__map, s.type, s.off,
                 claimable(s.off, extent_of(__base, __size, s.off, __version, s.type), __size));
            admitted = true;
        }
        std::vector<Abstraction::BlockRef> scratch;
        enumerate_one_level(s.off, s.type, w, scratch);
        __refs.insert(__refs.end(), scratch.begin(), scratch.end());
        for (const Abstraction::BlockRef& r : scratch) {
            if (r.target == k::NULL_OFFSET) continue;
            // REC-19.3 — recorded at the moment the walk sees it, and NOT a
            // gate: the descent rule below is deliberately independent.
            (void)audit_ref(__size, r, __failures);
            // THE STRICT DESCENT RULE (FastFHIR corroborated_tag): descend
            // while the self-offset is valid or within the flip budget AND
            // the child's own wire tag maps to the slot's declared type.
            //
            // Position-budget alone is not enough — it describes two
            // situations that must not be treated alike: a real child whose
            // VALIDATION took the flip (the stored word is a Hamming
            // neighbour of the address — within budget), or a PARENT slot
            // flipped onto arbitrary bytes whose shifted header happens to
            // land within budget (measured: an 8-flip coincidence, and
            // enumerating it invented references). The tag is the second
            // half of the redundancy; demand it.
            //
            // A child whose TAG is the damaged half fails the rule and is
            // NOT descended here: the edge is reported TagRepaired, and the
            // REC-19.7 reapply re-enumerates its subtree under the corrected
            // type behind the coherence gate — the tag-damaged case is the
            // reapply's job, exactly as in FastFHIR.
            if (r.expected == Abstraction::MAP_ENTRY_TILE_PIXEL_DATA) continue;  // streams are leaves
            if (self_repairable(__base, __size, r.target) &&
                tag_corroborates(__base, __size, r.target, r.expected))
                stack.push_back({r.target, r.expected, s.depth + 1});
        }
    }
    return admitted;
}

// ---------------------------------------------------------------------------
// scan(): extent per scanned block, by shape (RC-2.1) — the generated
// per-block extent() picks the primitive formula and the versioned header.
// ---------------------------------------------------------------------------

::Iris::File::Size extent_of(const BYTE* __base, ::Iris::File::Size __file_size, ::Iris::File::Offset __at,
                      std::uint32_t __version, Abstraction::MapEntryType __type) noexcept {
    using namespace b;
    switch (__type) {
        case Abstraction::MAP_ENTRY_FILE_HEADER:            return FILE_HEADER{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_TILE_TABLE:             return TILE_TABLE{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_CIPHER:                 return CIPHER{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_METADATA:               return METADATA{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_ATTRIBUTES:             return ATTRIBUTES{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_LAYER_EXTENTS:          return LAYER_EXTENTS{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_TILE_OFFSETS:           return TILE_OFFSETS{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_ATTRIBUTE_SIZES:        return ATTRIBUTE_SIZES{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_ATTRIBUTE_BYTES:        return ATTRIBUTE_BYTES{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_IMAGES:                 return IMAGES{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_IMAGE_BYTES:            return IMAGE_BYTES{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_ICC_PROFILE:            return ICC_PROFILE{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_ANNOTATIONS:            return ANNOTATIONS{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_ANNOTATION_BYTES:       return ANNOTATION_BYTES{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_ANNOTATION_GROUP_SIZES: return ANNOTATION_GROUP_SIZES{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_ANNOTATION_GROUP_BYTES: return ANNOTATION_GROUP_BYTES{__base, __at, __file_size, __version}.extent();
        case Abstraction::MAP_ENTRY_CLINICAL_METADATA:      return CLINICAL_METADATA{__base, __at, __file_size, __version}.extent();
        default: return 0;  // TILE_PIXEL_DATA streams and TILE_FRAME: sized by their own paths
    }
}

/// The full header a block of `__type` needs before its extent can be read:
/// extent() reads COUNT/STRIDE for arrays and byte runs, so the 10-byte
/// block-header fit is not enough. A candidate whose full header does not
/// fit is not a block the tiling can size — it is not noted, and its run
/// stays a hole. This is the guard that keeps a signature or a parent slot
/// within HEADER_SIZE of EOF from feeding extent_of an out-of-range read.
::Iris::File::Size extent_header_size(Abstraction::MapEntryType __type) noexcept {
    switch (__type) {
        case Abstraction::MAP_ENTRY_LAYER_EXTENTS:
        case Abstraction::MAP_ENTRY_TILE_OFFSETS:
        case Abstraction::MAP_ENTRY_ATTRIBUTE_SIZES:
        case Abstraction::MAP_ENTRY_IMAGES:
        case Abstraction::MAP_ENTRY_ANNOTATIONS:
        case Abstraction::MAP_ENTRY_ANNOTATION_GROUP_SIZES:
            return p::ArrayHeader::HEADER_SIZE;
        case Abstraction::MAP_ENTRY_ATTRIBUTE_BYTES:
        case Abstraction::MAP_ENTRY_IMAGE_BYTES:
        case Abstraction::MAP_ENTRY_ICC_PROFILE:
        case Abstraction::MAP_ENTRY_ANNOTATION_BYTES:
        case Abstraction::MAP_ENTRY_ANNOTATION_GROUP_BYTES:
        case Abstraction::MAP_ENTRY_CLINICAL_METADATA:
            return p::ByteArrayHeader::HEADER_SIZE;
        default:
            return p::BlockHeader::HEADER_SIZE;
    }
}

/// What a block may claim, given where it sits: no map entry may name bytes
/// the file does not have.
///
/// Extents are computed from wire fields — an ARRAY's COUNT, a tile entry's
/// SIZE — so damage makes them arbitrary, and a census that records the claim
/// verbatim hands every consumer a slice that runs off the end. The tiling
/// inherits it too: the run after an over-long entry starts past EOF. Zero
/// means "do not record this at all", which is the answer when the OFFSET
/// itself is outside the file (a corrupt tile entry can name 2^40 in an
/// 842-byte file). The overrun is not lost by clamping — it is exactly what
/// classify()'s ExtentDerived reports, from the block's own fields.
::Iris::File::Size claimable(::Iris::File::Offset __at, ::Iris::File::Size __extent,
                      ::Iris::File::Size __file_size) noexcept {
    if (static_cast<uint64_t>(__at) >= static_cast<uint64_t>(__file_size)) return 0;
    const ::Iris::File::Size room = static_cast<::Iris::File::Size>(__file_size - __at);
    return __extent > room ? room : __extent;
}

/// Best-effort declared version: the root when readable, else what this build
/// writes. Versioned extents and the version-skew gate both need it; a damaged
/// root must not poison either with garbage.
uint32_t declared_version(const BYTE* __base, ::Iris::File::Size __size) noexcept {
    const b::FILE_HEADER root = root_at(__base, __size);
    if (!root.validate()) return b::VERSION_WRITTEN;
    return p::compose_version(root.extension_major(), root.extension_minor());
}

}  // namespace

// ===========================================================================
// Recovery
// ===========================================================================

Recovery::Recovery(const FileAccessInfo& __info) noexcept
    : m_base(__info.file_ptr), m_size(__info.file_size) {}

std::vector<Abstraction::BlockRef> Recovery::reachable_blocks() const {
    // The clean-stream baseline: the offset-chain walk only. A damaged root
    // yields nothing — recover() still runs on the scan. The descent rule is
    // one-surviving-witness (RC-6): on a clean stream every child has both
    // witnesses, so this is identical to the pre-RC-6 walk.
    const b::FILE_HEADER root = versioned_root(m_base, m_size);
    if (!root.validate()) return {};
    std::unordered_set<Offset> visited;
    std::vector<Abstraction::BlockRef> refs;
    drive_walk(m_base, m_size,
               p::compose_version(root.extension_major(), root.extension_minor()),
               visited, refs, nullptr, nullptr, {{0, Abstraction::MAP_ENTRY_FILE_HEADER}});
    return refs;
}

Abstraction::FileMap Recovery::scan() const {
    using namespace Abstraction;
    FileMap map;
    map.file_size = m_size;
    const uint32_t version = declared_version(m_base, m_size);

    // The root has no VALIDATION, so the census cannot find it by signature;
    // note it explicitly or its own run reads as a Hole on every clean file.
    // A damaged root fails validate() and is left unnoted — the hole is then
    // honest.
    if (const b::FILE_HEADER root = versioned_root(m_base, m_size); root.validate())
        note(map, MAP_ENTRY_FILE_HEADER, 0, root.extent());

    constexpr Size SIGNATURE = p::BlockHeader::HEADER_SIZE;          // 10
    constexpr Size FRAME_SIGNATURE = p::FrameHeader::VALIDATION_SIZE;  // 5
    if (m_size < FRAME_SIGNATURE) {
        find_gaps(map);
        return map;
    }

    // RC-6 / REC-19.5 — THE PARALLEL CENSUS. The signature pass is per-byte
    // work with no shared state, so the arena is split across
    // hardware-concurrency chunks when it is large enough to amortize the
    // spawn; each worker emits its candidates locally, merged after join.
    // Chunks overlap by SIGNATURE-1 so a block straddling a boundary is seen
    // by the next chunk; duplicates are dropped at the audit (a map key is
    // claimed once). A worker exception would std::terminate if it escaped
    // the thread, so each captures its failure and it is rethrown on the
    // calling thread after join. The audit, tile charging and gap sweep stay
    // sequential.
    const auto emit_candidates = [&](Size begin, Size end,
                                     std::vector<Offset>& blocks,
                                     std::vector<Offset>& frames) {
        for (Size at = begin; at < end; ++at) {
            // Block signature first, frame second: a u64 self-offset at a
            // position is always also a u40 self-offset there, so the block
            // check must preempt — the original loop's `continue` did.
            if (at + SIGNATURE <= m_size &&
                ::Iris::File::load<std::uint64_t>(m_base + at) == static_cast<std::uint64_t>(at)) {
                blocks.push_back(static_cast<Offset>(at));
            } else if (at + FRAME_SIGNATURE <= m_size &&
                       ::Iris::File::load_u40(m_base + at) == static_cast<std::uint64_t>(at)) {
                frames.push_back(static_cast<Offset>(at));
            }
        }
    };

    const unsigned hw = std::thread::hardware_concurrency();
    const std::size_t workers = (m_size >= (1u << 20) && hw > 1)
                                    ? std::min<std::size_t>(hw, 8u) : 1;
    const std::size_t chunk = (static_cast<std::size_t>(m_size) + workers - 1) / workers;
    const std::size_t overlap = SIGNATURE - 1;
    std::vector<std::vector<Offset>> found_blocks(workers), found_frames(workers);
    if (workers == 1) {
        emit_candidates(0, m_size, found_blocks[0], found_frames[0]);
    } else {
        std::vector<std::exception_ptr> errors(workers);
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (std::size_t w = 0; w < workers; ++w) {
            const std::size_t begin = w * chunk;
            const std::size_t end = std::min(static_cast<std::size_t>(m_size),
                                             begin + chunk + (w + 1 < workers ? overlap : 0));
            threads.emplace_back([&, w, begin, end] {
                try {
                    emit_candidates(begin, end, found_blocks[w], found_frames[w]);
                } catch (const std::exception&) {
                    errors[w] = std::current_exception();
                }
            });
        }
        for (std::thread& t : threads) t.join();
        for (const auto& e : errors)
            if (e) std::rethrow_exception(e);
    }

    // The audit — single-threaded, preserving the original scan's semantics:
    // a valid block is noted as its tag's type; a self-consistent offset
    // whose tag is not a known type is RECORDED on map.failures (RC-6 /
    // REC-19.2) — never silently dropped — and still falls through to the
    // frame check below, exactly as the old loop did.
    std::vector<Offset> blocks, frames;
    for (const auto& v : found_blocks) blocks.insert(blocks.end(), v.begin(), v.end());
    for (const auto& v : found_frames) frames.insert(frames.end(), v.begin(), v.end());
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());

    for (const Offset at : blocks) {
        if (map.count(at)) continue;  // the root, or an overlap duplicate
        const k::RecoveryCodes tag = tag_at(m_base, m_size, at);
        if (const MapEntryType type = entry_for(tag); type != MAP_ENTRY_UNDEFINED) {
            // A block whose full header does not fit is not a block the
            // tiling can size — leave its run a hole rather than read past
            // the mapping for an extent.
            if (in_range(at, extent_header_size(type), m_size))
                note(map, type, at,
                     claimable(at, extent_of(m_base, m_size, at, version, type), m_size));
            continue;
        }
        map.failures.push_back({ProducerFailureKind::ScanTagInvalid, at, MAP_ENTRY_UNDEFINED,
                                tag, "self-offset is consistent but the recovery tag is not a known type"});
    }
    for (const Offset at : frames) {
        if (map.count(at)) continue;   // already claimed by a block — block wins
        const Offset stream_at = at + FRAME_SIGNATURE;
        if (map.count(stream_at)) continue;  // ditto: the block entry outlives the stream note
        const b::TILE_PIXEL_DATA frame{m_base, stream_at, m_size, b::VERSION_WRITTEN};
        if (!frame.validate()) continue;
        note(map, MAP_ENTRY_TILE_FRAME, stream_at - b::TILE_PIXEL_DATA::header_size,
             b::TILE_PIXEL_DATA::header_size);
        note(map, MAP_ENTRY_TILE_PIXEL_DATA, stream_at, 0);  // sized by the pass below
    }

    // A stream's extent lives only in its tile-offsets entry; charge the
    // streams of every scanned, self-consistent array. Entries that did not
    // survive leave their streams as holes — a stream without its entry has
    // no derivable length, and the frame does not carry one.
    std::vector<Offset> arrays;
    for (const auto& [off, e] : map)
        if (e.type == MAP_ENTRY_TILE_OFFSETS) arrays.push_back(off);
    for (const Offset off : arrays) {
        const b::TILE_OFFSETS offsets{m_base, off, m_size, version};
        if (!offsets.validate()) continue;
        for (uint32_t i = 0; i < offsets.count(); ++i) {
            const auto entry = offsets.entry(i);
            const Offset stream = static_cast<Offset>(entry.offset());
            if (stream == k::NULL_TILE || stream == k::NULL_OFFSET) continue;
            // A damaged entry names an arbitrary 40-bit address: charge only
            // what the file can hold, and nothing at all outside it.
            const Size charged = claimable(stream, entry.size_field(), m_size);
            if (charged != 0) note(map, MAP_ENTRY_TILE_PIXEL_DATA, stream, charged);
        }
    }

    find_gaps(map);
    return map;
}

void Recovery::find_gaps(Abstraction::FileMap& __map) const {
    __map.gaps.clear();
    if (__map.empty()) return;

    // VERSION GATE. A gap can only be benign skew if the file was written by
    // a newer engine than this reader; the FILE_HEADER carries the declared
    // version. Getting this backwards would reclassify real damage as benign,
    // which is strictly worse than reporting nothing.
    const uint32_t declared = declared_version(m_base, m_size);
    const bool newer_stream = declared > b::VERSION_WRITTEN;

    // Pass 1 — the raw runs, remembering which entry type each one trails.
    struct Raw { Offset start; Size len; Abstraction::MapEntryType after; };
    std::vector<Raw> raw;
    uint64_t cursor = 0;
    Abstraction::MapEntryType prev = Abstraction::MAP_ENTRY_UNDEFINED;
    for (const auto& [off, e] : __map) {
        const uint64_t o = static_cast<uint64_t>(off);
        if (o > cursor) raw.push_back({static_cast<Offset>(cursor),
                                       static_cast<Size>(o - cursor), prev});
        cursor = std::max(cursor, o + static_cast<uint64_t>(e.size));
        prev = e.type;
    }
    if (static_cast<uint64_t>(__map.file_size) > cursor)
        raw.push_back({static_cast<Offset>(cursor),
                       static_cast<Size>(__map.file_size - cursor), prev});

    // Pass 2 — systematicity per type: a tag whose every instance trails the
    // same run is a version delta, not N separate holes. Self-calibrating:
    // the reader cannot know the newer layout, but it can observe 84 tiles
    // all trailing the same run and conclude the delta.
    std::unordered_map<uint16_t, std::size_t> instances;
    for (const auto& [off, e] : __map) instances[static_cast<uint16_t>(e.type)]++;
    std::unordered_map<uint16_t, std::unordered_map<Size, std::size_t>> trail;
    for (const Raw& r : raw)
        if (r.after != Abstraction::MAP_ENTRY_UNDEFINED)
            trail[static_cast<uint16_t>(r.after)][r.len]++;

    for (const Raw& r : raw) {
        Abstraction::Gap g{r.start, r.len, Abstraction::GapClass::Hole};
        if (static_cast<uint64_t>(r.start) + r.len >=
            static_cast<uint64_t>(__map.file_size)) {
            g.class_ = Abstraction::GapClass::Trailing;  // arena slack past the last entry
        } else if (newer_stream && r.after != Abstraction::MAP_ENTRY_UNDEFINED) {
            const uint16_t t = static_cast<uint16_t>(r.after);
            const auto it = trail.find(t);
            const std::size_t same =
                (it != trail.end() && it->second.count(r.len)) ? it->second.at(r.len) : 0;
            // Every instance of the tag trails this exact run → a layout the
            // writer knows and this reader does not.
            if (same > 1 && same == instances[t]) g.class_ = Abstraction::GapClass::VersionSkew;
        }
        __map.gaps.push_back(g);
    }
}

namespace {

/// Whether a map entry type is an array block (extent = header + stride×count;
/// the header is 16 bytes for all six, and COUNT lives at ArrayHeader::COUNT).
bool is_array_type(Abstraction::MapEntryType __type) noexcept {
    switch (__type) {
        case Abstraction::MAP_ENTRY_LAYER_EXTENTS:
        case Abstraction::MAP_ENTRY_TILE_OFFSETS:
        case Abstraction::MAP_ENTRY_ATTRIBUTE_SIZES:
        case Abstraction::MAP_ENTRY_IMAGES:
        case Abstraction::MAP_ENTRY_ANNOTATIONS:
        case Abstraction::MAP_ENTRY_ANNOTATION_GROUP_SIZES:
            return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// The repair ranker's search space (FastFHIR REC-20.2/.4, ported as RC-7)
// ---------------------------------------------------------------------------

/// One position inside a hole whose residual self-offset word is a Hamming
/// near-neighbour of its own address. A block encodes its own offset, so a
/// damaged VALIDATION word still reads close to where it sits — these are the
/// positions a broken reference's true child may occupy.
///
/// The hole evidence this replaces was SIZED: a hole matched a reference only
/// when its length equalled the declared type's fixed extent, which assumed
/// the lost block started at the hole's first byte and failed whenever one
/// hole held more than one lost block (FastFHIR REC-18.6 → REC-20.2). The
/// residual word is stronger evidence, and it localises the block INSIDE the
/// hole, at any position of it.
struct HoleCandidate {
    Offset   pos;        ///< candidate child address inside the hole — the
                         ///< location: the noise-free expected address
    uint32_t self_cost;  ///< hamming(word_at(pos), pos) — the distance between
                         ///< the location and its own offset encoding
    uint16_t tag;        ///< the two residual RECOVERY bytes at pos
};

/// The stored word itself is CONSUMED at admission and deliberately not kept.
/// Its only use is `self_cost` — how likely this position is to be a real
/// block whose VALIDATION was damaged. It is never a matching target: the
/// parent's damaged offset is scored against `pos`, which is exact. A field
/// holding the word is how someone hams two damaged copies of one number by
/// mistake, which measured strictly worse (15,946 vs 15,944 in FastFHIR).

/// REC-20.2 + RC-10 double hamming — the ranked candidate list, swept once
/// per recover() call and once per band. Every position of every Hole run
/// whose residual block signature is within `__band` bits of the nine bytes a
/// block header would put there, strongest self-similarity first.
///
/// THE DOUBLE HAMMING. A block start is nine constant-ish bytes:
/// the u64 VALIDATION word (== own address) followed by the tag's 0x55 prefix
/// byte. A lost block usually carries ONE damaged half — the flip that
/// destroyed its VALIDATION word leaves the tag bytes alone — so admission
/// costs BOTH residuals against their constants:
///
///     self_cost  = hamming(word_at(pos), pos)           // 8 bytes
///     prefix     = hamming(prefix_byte_at(pos), 0x55)   // the 9th byte
///     admit when self_cost + prefix <= __band
///
/// The prefix byte is nearly free signal for a real block and costs a full
/// flip (or more) against the coincidence floor: hole bytes are not random —
/// the arena is full of 8-byte offset words that share their high bits with
/// their own position — so a word-only threshold harvests that structure as
/// candidates and turns clean verdicts Ambiguous. Measured over 12,227 hole
/// bytes on a 512-flip artifact, the word-only histogram was 1:35 2:14 3:7
/// 4:15 5:14 6:15 7:10 8:10 — a spike at 1–2 (real lost blocks) on a flat
/// floor from 3 up. Adding the prefix byte pushes the floor out by the tag
/// damage real blocks almost never carry.
///
/// WIRE POSITION OF THE PREFIX BYTE. The u16 tag is little-endian, so the
/// constant 0x55 is the HIGH byte of the word at pos+8 — byte pos+9, not
/// pos+8 (pos+8 holds the type-specific low byte). The typed load below is
/// what keeps `tag >> 8` the right byte on every host; never index the
/// buffer by hand here.
///
/// THE BAND MUST START TIGHT, and the widening loop is what rescues a block
/// whose WORD took 3+ flips: prefix-intact candidates admit at the same band
/// as before (the word term is unchanged), so a real hole that lost only its
/// VALIDATION — the usual shape — is not deferred. A block whose tag-prefix
/// byte ALSO flipped (word + prefix over the band) waits for the widened
/// pass, which is the same price the coincidence floor pays; the floor is
/// what the widening loop exists to outlast.
///
/// `self_cost` stays the WORD distance only: it is a separate term of the
/// per-reference match metric, which prices the FULL residual tag (prefix and
/// type bytes) against the slot's declared code — folding the prefix byte
/// into `self_cost` would charge the same flip twice.
std::vector<HoleCandidate> collect_hole_candidates(const Abstraction::FileMap& __map,
                                                   const BYTE* __base,
                                                   std::uint32_t __band) noexcept {
    std::vector<HoleCandidate> out;
    for (const Abstraction::Gap& g : __map.gaps) {
        if (g.class_ != Abstraction::GapClass::Hole ||
            g.length < p::BlockHeader::HEADER_SIZE)
            continue;  // too small to have held a block header — noise
        const uint64_t last = static_cast<uint64_t>(g.start) + g.length
                            - p::BlockHeader::HEADER_SIZE;
        for (uint64_t pos = static_cast<uint64_t>(g.start); pos <= last; ++pos) {
            if (pos + p::BlockHeader::HEADER_SIZE
                > static_cast<uint64_t>(__map.file_size))
                break;
            const uint64_t word = ::Iris::File::load<std::uint64_t>(
                __base + static_cast<Offset>(pos));
            const uint16_t tag = ::Iris::File::load<std::uint16_t>(
                __base + static_cast<Offset>(pos) + p::BlockHeader::RECOVERY);
            const uint32_t word_cost = Recovery::hamming_cost(word, pos);
            const uint32_t prefix_cost = Recovery::hamming_cost(
                static_cast<uint64_t>(tag >> 8),
                static_cast<uint64_t>(p::RECOVERY_TAG_PREFIX));
            // Admission is the DOUBLE HAMMING; self_cost (the match metric's
            // separate term) stays the word distance only — the match prices
            // the full residual tag itself.
            if (word_cost + prefix_cost > __band) continue;
            out.push_back({static_cast<Offset>(pos), word_cost, tag});
        }
    }
    // Cheapest first; position breaks the tie so the order is deterministic.
    // A classifier stops caring about the tail once costs exceed the budget,
    // which is what keeps the per-reference match cheap.
    std::sort(out.begin(), out.end(),
              [](const HoleCandidate& a, const HoleCandidate& b) {
                  if (a.self_cost != b.self_cost) return a.self_cost < b.self_cost;
                  return a.pos < b.pos;
              });
    return out;
}

/// Two gate strengths, one predicate (FastFHIR BatchTest, ported RC-8): how
/// strictly must a freshly enumerated batch of references hold up? A family
/// of near-identical functions is how a caller ends up picking by vibe, and
/// using the wrong strength does not fail loudly — it silently disables the
/// caller, which is how the generational cascade stayed broken.
enum class BatchTest : uint8_t {
    /// Every child must corroborate — an exact self-offset, or a wire tag that
    /// maps to the slot's declared type. The TYPE is a hypothesis here (a
    /// wrong V-Table yields nothing but nonsense), so the batch must go whole.
    EveryChildCorroborates,
    /// No child may be a wild pointer; damage is expected. The type is already
    /// CORROBORATED, so the V-Table is trusted and the only question left is
    /// whether the read produced wild pointers.
    NoWildPointers,
};

/// Demanding corroboration where only addressability is warranted is what
/// silenced the generational cascade: a block whose one child is itself
/// damaged has no corroborating child BY DEFINITION, so its batch was
/// discarded and the chain stopped at the first repair. Parent broken, child
/// broken, grandchild broken is exactly what that loop exists to unwind.
///
/// A wrong V-Table still cannot survive NoWildPointers: it lifts offsets out
/// of positions that hold none, and those are overwhelmingly outside the
/// stream. One that happens to land in bounds yields references that classify
/// as Unrecovered — reported, never believed.
bool batch_passes(const BYTE* __base, Size __size,
                  const std::vector<Abstraction::BlockRef>& __refs,
                  BatchTest __test) noexcept {
    for (const Abstraction::BlockRef& c : __refs) {
        if (c.target == k::NULL_OFFSET) continue;             // absence, not damage
        if (c.expected == Abstraction::MAP_ENTRY_TILE_PIXEL_DATA) continue;  // streams carry no header
        if (!in_range(c.target, p::BlockHeader::HEADER_SIZE, __size)) return false;  // a wild pointer fails both
        if (__test == BatchTest::NoWildPointers) continue;
        if (static_cast<Offset>(::Iris::File::load<std::uint64_t>(__base + c.target)) == c.target)
            continue;
        if (c.recovery != k::RecoveryCodes::RECOVER_UNDEFINED
            && Abstraction::entry_for(c.recovery) == c.expected)
            continue;
        return false;  // a child with no surviving witness — garbage
    }
    return true;
}

/// Does the block at `__off` READ COHERENTLY as `__type`? (FastFHIR
/// reads_as, REC-22.2, ported RC-8.)
///
/// A type is a hypothesis about a V-Table. Enumerate the block's slots under
/// that hypothesis and every child must corroborate: a wrong V-Table reads
/// real offsets out of positions that hold something else, and what comes
/// back does not vouch for itself. A block with no enumerable children
/// answers NEITHER way — that is not evidence, and it reports as such rather
/// than passing vacuously.
bool block_reads_as(const BYTE* __base, Size __size, std::uint32_t __version,
                    Offset __off, Abstraction::MapEntryType __type) noexcept {
    if (__off == k::NULL_OFFSET || __type == Abstraction::MAP_ENTRY_UNDEFINED)
        return false;
    Walker w{__base, __size, __version};
    std::vector<Abstraction::BlockRef> kids;
    enumerate_one_level(__off, __type, w, kids);
    if (kids.empty()) return false;  // silence, not agreement
    return batch_passes(__base, __size, kids, BatchTest::EveryChildCorroborates);
}

/// THE HOLE-WINNER SANITY GATE (mechanism 2): would a block at `__off` of
/// type `__type` read as anything but noise? Enumerate ONE level under the
/// type and require no wild pointers — the user's "does it produce a bunch
/// of nonsense offsets to the grandchildren" test, applied BEFORE a hole
/// candidate is admitted as a repoint winner.
///
/// This is deliberately WEAKER than reads_as in one direction and stronger
/// in another: a candidate whose own VALIDATION is damaged (the reason it is
/// a hole) still enumerates its real children, whose offsets are in range —
/// the children classify separately, damaged ones included, so the block is
/// not rejected for being the very damage being hunted. But floor junk — a
/// coincidence whose word hams well against its address — has no real slots:
/// its "children" are offsets lifted from noise, overwhelmingly outside the
/// stream, and NoWildPointers refuses it. A type with no enumerable
/// children answers NEITHER way (leaf: the hamming match and the header
/// audit must vouch alone) and is accepted — silence is not rejection here,
/// unlike reads_as, because a real parent whose slots are all NULL_OFFSET
/// enumerates nothing and is not junk.
/// THE WINNER COHERENCE GATE (the "does it produce nonsense offsets to the
/// grandchildren" check). A hole candidate that hams well against a broken
/// reference is still a hypothesis — a position whose residual word happens
/// to sit within budget of the damaged slot. Before a repoint may land there,
/// enumerate one level under the presumed type: a real parent's slots name
/// real in-file targets, while floor junk lifts "offsets" out of positions
/// that hold none. The test is EXISTENTIAL, not universal: at least one
/// child target must land in the file. Requiring every child in-range would
/// reject the true parent whose OWN child slot took a second flip (a 1-bit
/// flip in a child slot's high byte names an address near 2^40 — out of
/// range — while its siblings stay real); that child is exactly what the
/// widened bands exist to hunt, so the parent must be admitted for them to
/// see it. Junk fails the existential test with probability ~1: random
/// u64s land in a file of a few KB almost never. A type with no enumerable
/// children answers NEITHER way (empty is no opinion, not a rejection — a
/// real empty parent must not be refused); its header audit and the hamming
/// match vouch alone.
bool winner_reads_sane(const BYTE* __base, Size __size, std::uint32_t __version,
                       Offset __off, Abstraction::MapEntryType __type) noexcept {
    if (__off == k::NULL_OFFSET || __type == Abstraction::MAP_ENTRY_UNDEFINED ||
        __type == Abstraction::MAP_ENTRY_TILE_PIXEL_DATA)
        return false;
    if (!in_range(__off, p::BlockHeader::HEADER_SIZE, __size)) return false;
    Walker w{__base, __size, __version};
    std::vector<Abstraction::BlockRef> kids;
    enumerate_one_level(__off, __type, w, kids);
    for (const Abstraction::BlockRef& kid : kids) {
        if (kid.target == k::NULL_OFFSET) continue;   // absence, not damage
        if (in_range(kid.target, p::BlockHeader::HEADER_SIZE, __size))
            return true;                              // one real child — a real parent
    }
    return kids.empty();  // no opinion (a leaf, or an empty parent) — not rejection
}

/// The TILE_INDEX a framed stream claims about itself. Absent when the anchor
/// carries no frame: an unframed stream answers neither way, and must never be
/// rejected for failing to answer.
///
/// Read from the bytes rather than from BlockRef::recovery, which narrows this
/// u32 to the u16 the RecoveryCodes enum is: two tiles 65,536 apart share a
/// truncated index, and this comparison is only sound on the full value.
std::optional<std::uint32_t> frame_tile_index(const BYTE* __base, Size __size,
                                              Offset __anchor) noexcept {
    constexpr Size BACK = p::FrameHeader::VALIDATION_SIZE
                        + b::TILE_PIXEL_DATA::size::TILE_INDEX;
    if (__anchor < BACK) return std::nullopt;
    if (!p::FrameHeader::signature_matches(__base, __anchor, __size)) return std::nullopt;
    return ::Iris::File::load<std::uint32_t>(__base + __anchor - BACK);
}

/// The tile index an ENTRY owns, from where the entry sits in its array. The
/// index is arithmetic over the parent's own header, so it carries no wire
/// field of its own to be damaged. Absent when that header cannot be read or
/// the slot does not land on an entry boundary — an index derived from garbage
/// would reject correct candidates, which is the one outcome this check exists
/// to prevent.
std::optional<std::uint32_t> entry_tile_index(const BYTE* __base, Size __size,
                                              std::uint32_t __version,
                                              const Abstraction::BlockRef& __r) noexcept {
    const b::TILE_OFFSETS offsets{__base, __r.parent, __size, __version};
    if (!offsets.in_bounds()) return std::nullopt;
    const std::uint16_t stride = offsets.stride();
    const Offset begin = offsets.entries_begin();
    const std::uint64_t at = static_cast<std::uint64_t>(__r.parent) + __r.slot;
    if (stride == 0 || at < begin) return std::nullopt;
    const std::uint64_t within = at - begin;
    if (within % stride != 0) return std::nullopt;
    return static_cast<std::uint32_t>(within / stride);
}

/// One verdict, computed from the two witnesses, the orphan census and the
/// ranked hole-candidate pool. Never guesses: two hypotheses at equal cost is
/// Ambiguous with its candidates; nothing within budget is Unrecovered.
void classify(Abstraction::BlockVerdict& __v, const Abstraction::FileMap& __map,
              const std::unordered_map<uint16_t, std::vector<Offset>>& __orphans,
              const std::vector<HoleCandidate>& __pool, const BYTE* __base,
              std::uint32_t __version) {
    const Abstraction::BlockRef& r = __v.block;

    // The tile edge is the format's one headerless child: witness 2 is the
    // optional frame. Framed and self-consistent -> Intact. Otherwise rank
    // the frame-witnessed orphan census: a stream the frame notes but no
    // entry names is the true child of a broken entry (RC-3.4) — the frame's
    // forty-bit self-offset restores the entry's position.
    if (r.expected == Abstraction::MAP_ENTRY_TILE_PIXEL_DATA) {
        // A stream in the first five bytes of the file cannot carry a frame,
        // and the subtraction must not be performed there: target == 4
        // underflows to all-ones, which is exactly the value an ABSENT
        // witness holds — an entry corrupted to 4 would read as Intact.
        const bool framable =
            static_cast<uint64_t>(r.target) >= p::FrameHeader::VALIDATION_SIZE;
        const Offset framed = framable
            ? static_cast<Offset>(static_cast<uint64_t>(r.target)
                                  - p::FrameHeader::VALIDATION_SIZE)
            : k::NULL_OFFSET;
        if (framable && r.validation == framed) {
            __v.class_ = Abstraction::RepairClass::Intact;
            return;
        }
        const auto it = __orphans.find(
            static_cast<uint16_t>(Abstraction::MAP_ENTRY_TILE_PIXEL_DATA));
        if (it != __orphans.end() && !it->second.empty()) {
            // THE FRAME'S TILE_INDEX IS AN EXACT IDENTITY WITNESS, and it is
            // the only one this format gives a headerless child. Everything
            // else here is an ESTIMATE: hamming distance says how far a
            // corrupted offset sits from a candidate, and the claim boundary
            // says a candidate's run tiles the arena — neither says the
            // candidate is THIS tile. The index does, which is the whole
            // reason the field exists (a stream cannot recover its own place
            // in the grid, and streams are written in any order).
            //
            // FastFHIR reached for statistical LOCALITY here — bracket a
            // candidate between the nearest intact neighbours in parent order
            // — because FHIR had no such field. That rule does not port:
            // Iris-Codec claims a tile's arena space with fetch_add AFTER
            // compression returns, on hardware_concurrency() workers, so a
            // stream's offset records completion order, not tile order.
            // Measured over that claim discipline: tile offsets ascend with
            // index 57% of the time at 18 threads, and bracketing rejects
            // 62% of CORRECT repoints. The index is exact where locality is
            // not even indicative.
            //
            // NARROWING ONLY. A candidate is dropped when it carries a frame
            // that names a DIFFERENT tile; a candidate with no frame is not
            // evidence and survives. So this can only remove wrong answers,
            // never admit one the ranker would have refused — and when it
            // empties the pool the verdict falls through to Unrecovered
            // below, which is the honest outcome: a pool where nothing claims
            // to be this tile does not contain this tile.
            //
            // Exact match, never hamming. Adjacent indices differ by ONE bit
            // (tile 4 and tile 5), so a distance test over an identity would
            // re-admit exactly the neighbour confusion being removed.
            const std::optional<std::uint32_t> own =
                entry_tile_index(__base, __map.file_size, __version, r);
            uint32_t best = IFE_RECOVERY_MAX_FLIPS + 1;
            std::vector<Offset> best_list;
            for (const Offset cand : it->second) {
                if (own) {
                    const std::optional<std::uint32_t> claimed =
                        frame_tile_index(__base, __map.file_size, cand);
                    if (claimed && *claimed != *own) continue;
                }
                // Size validation (RC-3.4): the entry's claimed extent must
                // end at a claim boundary — a claim of the arena (the
                // corrupted entry's own phantom charge included), or EOF.
                // The frame carries no length, so the claim is the only
                // size the reconciliation has; a wrong stream of a
                // different size ends mid-claim and is not the child. An
                // implausible claim (larger than the file) is ignored and
                // ranking falls back to hamming distance alone.
                if (r.extent != 0) {
                    const std::uint64_t end =
                        static_cast<std::uint64_t>(cand) + r.extent;
                    if (end > static_cast<std::uint64_t>(__map.file_size)) continue;
                    if (end != static_cast<std::uint64_t>(__map.file_size) &&
                        !__map.count(static_cast<Offset>(end)))
                        continue;
                }
                const uint32_t cost = Recovery::hamming_cost(
                    static_cast<uint64_t>(cand), static_cast<uint64_t>(r.target));
                if (cost < best) { best = cost; best_list = {cand}; }
                else if (cost == best) best_list.push_back(cand);
            }
            if (best <= IFE_RECOVERY_MAX_FLIPS) {
                if (best_list.size() > 1) {
                    __v.class_ = Abstraction::RepairClass::Ambiguous;
                    __v.candidates = std::move(best_list);
                } else {
                    __v.class_ = Abstraction::RepairClass::Corroborated;
                    __v.bit_cost = best;
                    __v.repaired = best_list.front();
                }
                return;
            }
            // A frame witness exists but none is within budget: the entry is
            // broken beyond repair — reported, never silently Intact.
            __v.class_ = Abstraction::RepairClass::Unrecovered;
            return;
        }
        // No frame witness at all: an unframed entry is its own only witness,
        // so nothing contradicts the claim and it stands — a destroyed stream
        // reads as a Hole, never as a guess.
        if (r.validation == k::NULL_OFFSET) {
            __v.class_ = Abstraction::RepairClass::Intact;
            return;
        }
        __v.class_ = Abstraction::RepairClass::Unrecovered;
        return;
    }

    const bool pos_ok = r.validation == r.target;
    const bool tag_ok = r.recovery != k::RecoveryCodes::RECOVER_UNDEFINED
                        && Abstraction::entry_for(r.recovery) == r.expected;

    // TAG CONSENSUS (FastFHIR REC-22.2, ported RC-8): the THIRD OPINION.
    //
    // The child validates but carries a tag of another PLausible type: two
    // situations wear one face — the child's TAG was flipped, or the PARENT's
    // offset was flipped onto an innocent, perfectly valid block of another
    // type. The ranker below costs both hypotheses at the tag distance and
    // breaks the tie by cost, which can pick wrong — and a wrong tag REWRITE
    // relabels real data (item 18). So do not decide it by cost. Ask whether
    // the block READS as each candidate: a type is a hypothesis about a
    // V-Table, and the wrong one pulls offsets out of positions holding
    // something else. Exactly one coherent reading is an answer; anything
    // else is no opinion and falls through to the ranker.
    //
    //   reads as the slot's type, not its own  -> the child's tag is the
    //       damaged half: adjudicated TagRepaired, and apply() may rewrite a
    //       plausible tag because evidence, not cost, decided it.
    //   reads as its own type, not the slot's  -> an innocent block: the
    //       parent's offset is the damaged half. A tag rewrite would relabel
    //       real data, so the in-place hypothesis is disabled and only a
    //       repoint may fix the edge.
    bool innocent_block = false;
    if (pos_ok && !tag_ok) {
        const Abstraction::MapEntryType own = Abstraction::entry_for(r.recovery);
        if (own != Abstraction::MAP_ENTRY_UNDEFINED) {
            const bool reads_exp = block_reads_as(
                __base, __map.file_size, __version, r.target, r.expected);
            const bool reads_own = block_reads_as(
                __base, __map.file_size, __version, r.target, own);
            if (reads_exp && !reads_own) {
                __v.class_ = Abstraction::RepairClass::TagRepaired;
                __v.bit_cost = Recovery::hamming_cost(
                    static_cast<uint64_t>(r.recovery),
                    static_cast<uint64_t>(Abstraction::recovery_for(r.expected)));
                __v.tag_adjudicated = true;
                return;
            }
            if (reads_own && !reads_exp) innocent_block = true;
        }
    }

    if (pos_ok && tag_ok) { __v.class_ = Abstraction::RepairClass::Intact; }
    else {
        // TWO DAMAGED-WITNESS HYPOTHESES, RANKED ON ONE METRIC (FastFHIR
        // REC-19.6 / REC-20.4, ported as RC-7):
        //
        //   H_inplace rewrites the child's OWN bytes at the parent-named
        //     address — the RECOVERY tag when the child validates but is
        //     mislabelled (TagRepaired), or the VALIDATION word when the bytes
        //     there still carry the slot's type but no longer self-validate
        //     (PositionRepaired). When the tag is ALSO gone the address is not
        //     corroborated as this block, and rewriting its VALIDATION could
        //     relabel an innocent block of another type — only a repoint may
        //     fix that edge.
        //   H_repoint rewrites the PARENT's stored offset to the cheapest
        //     unique candidate — an orphan (a self-consistent block no slot
        //     names: "I am here") or a ranked hole position (a residual
        //     self-offset: "something was here").
        //
        // The hypotheses used to be decided by shape, not by cost: a child
        // that validated but carried the wrong tag was ALWAYS TagRepaired,
        // even when a cheaper unique repoint said the parent's offset was the
        // damaged half — a flip landing on an innocent, perfectly valid block
        // of another type reads exactly like a flipped tag, and only the
        // ranker separates them. The hole match was also offered only when no
        // orphan was in budget, so the two pools never competed on one scale.
        // Cheapest under the flip budget wins; equal costs are Ambiguous.
        const uint16_t declared_code =
            static_cast<uint16_t>(Abstraction::recovery_for(r.expected));
        uint32_t h_inplace = IFE_RECOVERY_MAX_FLIPS + 1;
        if (pos_ok && !innocent_block) {
            // Child VALIDATION intact, tag wrong: the slot's compiled
            // expectation rewrites it (mode 2b). Skipped for an adjudicated
            // innocent block — the bytes read as their own type, so the
            // parent's offset is the damaged half and rewriting the tag would
            // relabel real data.
            h_inplace = Recovery::hamming_cost(
                static_cast<uint64_t>(r.recovery),
                static_cast<uint64_t>(declared_code));
        } else if (tag_ok) {
            // Child tag intact, VALIDATION wrong: the parent-named address
            // recomputes it — position-verified, content NOT verified
            // (mode 2a). r.recovery != RECOVER_UNDEFINED guarantees the word
            // was read (read_witnesses reads both halves under one bound).
            h_inplace = Recovery::hamming_cost(
                static_cast<uint64_t>(r.validation),
                static_cast<uint64_t>(r.target));
        }

        uint32_t h_off = IFE_RECOVERY_MAX_FLIPS + 1;
        bool off_unique = false;    // exactly one cheapest candidate
        bool off_is_hole = false;   // it came from a hole, not an orphan
        __v.candidates.clear();
        const auto consider = [&](Offset pos, uint32_t cost, bool from_hole) {
            if (cost < h_off) {
                h_off = cost; off_unique = true; off_is_hole = from_hole;
                __v.candidates.assign(1, pos);
            } else if (cost == h_off) {
                off_unique = false;                 // a tie — never guessed
                __v.candidates.push_back(pos);
            }
        };

        // H_repoint, pool 1: the orphans of the declared type. Scored on the
        // offset distance alone — a self-consistent block validates its own
        // offset and its tag is exact by bucket construction, so the other
        // two terms would be theatre.
        const auto it = __orphans.find(static_cast<uint16_t>(r.expected));
        if (it != __orphans.end())
            for (const Offset cand : it->second)
                consider(cand,
                         Recovery::hamming_cost(static_cast<uint64_t>(r.target),
                                                static_cast<uint64_t>(cand)),
                         /*from_hole=*/false);

        // H_repoint, pool 2: the ranked hole positions, SAME metric, with the
        // candidate's own residual damage carried as separate evidence — the
        // offset term is scored against the candidate's EXACT position
        // (noise-free), never against another damaged copy of the same word.
        // Comparing one noisy observation to a known value beats comparing
        // two noisy observations (FastFHIR measured tuple-vs-tuple strictly
        // worse); the extra terms are what keep a clean orphan outranking a
        // hole that needs the same offset correction. A position that fails
        // the coherence gate is floor junk wearing a hamming suit — its
        // "grandchildren" are wild pointers — and must not win the repoint;
        // leaving it out lets the widened bands see the TRUE child, whose
        // deeper self-cost (3+) the tight band cannot admit (measured: a
        // block whose VALIDATION took three flips lost its whole subtree to
        // a band-2 coincidence repoint).
        for (const HoleCandidate& hc : __pool) {
            if (__map.count(hc.pos)) continue;  // claimed since the pool was built
            if (!winner_reads_sane(__base, __map.file_size, __version,
                                   hc.pos, r.expected))
                continue;  // nonsense grandchildren — never guessed
            consider(hc.pos,
                     Recovery::hamming_cost(static_cast<uint64_t>(r.target),
                                            static_cast<uint64_t>(hc.pos))
                         + hc.self_cost
                         + Recovery::hamming_cost(static_cast<uint64_t>(hc.tag),
                                                  static_cast<uint64_t>(declared_code)),
                     /*from_hole=*/true);
        }

        const bool in_place = h_inplace <= IFE_RECOVERY_MAX_FLIPS;
        const bool in_off = off_unique && h_off <= IFE_RECOVERY_MAX_FLIPS;
        if (in_place && in_off && h_inplace == h_off) {
            // Rewriting the child and repointing the parent cost the same —
            // two live readings: reported with its candidates, never guessed.
            __v.class_ = Abstraction::RepairClass::Ambiguous;
            __v.bit_cost = h_inplace;
            return;
        }
        if (in_place && (!in_off || h_inplace < h_off)) {
            __v.class_ = pos_ok ? Abstraction::RepairClass::TagRepaired
                                : Abstraction::RepairClass::PositionRepaired;
            __v.bit_cost = h_inplace;
            return;
        }
        if (in_off) {
            __v.class_ = off_is_hole ? Abstraction::RepairClass::HoleCorroborated
                                     : Abstraction::RepairClass::Corroborated;
            __v.bit_cost = h_off;
            __v.repaired = __v.candidates.front();
            return;
        }
        __v.class_ = Abstraction::RepairClass::Unrecovered;
        return;
    }

    // ExtentDerived — an array whose claimed extent overruns the next entry
    // or the file: the tiling knows the true limit, so the extent is
    // recomputed from it rather than from the corrupt COUNT.
    if (__v.class_ == Abstraction::RepairClass::Intact && is_array_type(r.expected)) {
        const auto it = __map.find(r.target);
        if (it != __map.end() && it->second.size != 0) {
            const uint64_t claimed_end = static_cast<uint64_t>(r.target) + it->second.size;
            uint64_t limit = static_cast<uint64_t>(__map.file_size);
            const auto next = __map.upper_bound(r.target);
            if (next != __map.end() && static_cast<uint64_t>(next->first) < claimed_end)
                limit = static_cast<uint64_t>(next->first);
            const Size hdr = p::ArrayHeader::HEADER_SIZE;
            const bool readable = in_range(r.target, hdr, __map.file_size)
                               && it->second.size >= hdr;
            const uint32_t stride = readable
                ? ::Iris::File::load<std::uint16_t>(__base + r.target + p::ArrayHeader::STRIDE) : 0;
            // A zero STRIDE is not a divisor. It also cannot reach here from a
            // real array — extent() would report header-only and nothing would
            // overrun — but the guard is a byte, and this runs on damaged bytes.
            // The recomputed extent must still admit a header, or the repair
            // is one apply() would refuse — report nothing rather than a
            // verdict that cannot be written.
            const bool sized = limit >= static_cast<uint64_t>(r.target) + hdr;
            if (claimed_end > limit && readable && sized && stride != 0) {
                const Size recomputed = static_cast<Size>(limit - static_cast<uint64_t>(r.target));
                const uint32_t claimed_count = static_cast<uint32_t>((it->second.size - hdr) / stride);
                const uint32_t fixed_count  = static_cast<uint32_t>((recomputed - hdr) / stride);
                __v.class_ = Abstraction::RepairClass::ExtentDerived;
                __v.bit_cost = Recovery::hamming_cost(claimed_count, fixed_count);
                __v.repaired = static_cast<Offset>(recomputed);
            }
        }
    }
}

}  // namespace

Abstraction::RecoveryReport Recovery::recover() const {
    using namespace Abstraction;
    RecoveryReport report;
    std::vector<ProducerFailure> failures;
    std::unordered_set<Offset> visited;
    std::vector<BlockRef> refs;

    // ROOT-HEADER PROBE — the root is the one structure whose correct bytes
    // are knowable WITHOUT a wire witness, because it is the header: its
    // shape is fixed and known before it is read. MAGIC and the RECOVERY tag
    // are constants the format defines (IFE_Blocks: "'Iris'"), and the
    // extension version must be one this reader knows (append-only: every
    // minor of the current major down to 0). Each field damaged within the
    // flip budget earns a RootRepaired verdict — a write of a KNOWN value,
    // the strongest evidence the engine has (stronger than a ranked
    // candidate, which is why it is not bounded by the same doubt). A root
    // no reference reaches makes the whole file unreachable: T00-style
    // damage in the bench — a flip in bytes 0..5 of an 832-byte file — lost
    // all 18 units because nothing repaired the header. The probe runs
    // FIRST so the corrected version drives every read below: a garbage
    // version field would otherwise mis-size the census's tiling. Today
    // every supported version shares one layout ("Version 1.0 ends here"),
    // so the corrected version changes no sizes yet — the write still
    // matters for the version gate's honesty and for the next minor that
    // appends fields.
    std::vector<BlockVerdict> root_verdicts;
    uint32_t version = declared_version(m_base, m_size);
    const b::FILE_HEADER root = versioned_root(m_base, m_size);
    const bool root_fits = root.in_bounds();
    if (root_fits) {
        // One header feature probed against its known value. The header is
        // the one structure whose correct bytes are knowable without a wire
        // witness, and FOUR features are knowable a priori: MAGIC and the
        // RECOVERY tag are constants the format defines, FILE_SIZE must
        // equal the mapping's size (known from the OS for a file on disk —
        // this engine is always constructed over the mapped bytes, so
        // `m_size` IS the OS size), and the extension version must be one
        // this reader knows. Each field within the flip budget earns a
        // RootRepaired verdict — a write of a KNOWN value, the strongest
        // evidence the engine has. Returns the field's hamming distance so
        // the identity gate below can see which features survived.
        const auto probe_field = [&](Offset __at, std::uint64_t __want,
                                     Size __width) -> std::uint32_t {
            std::uint64_t now = 0;
            for (Size i = 0; i < __width; ++i)
                now |= static_cast<std::uint64_t>(m_base[__at + i]) << (8u * i);
            const std::uint32_t cost = Recovery::hamming_cost(now, __want);
            if (cost == 0 || cost > IFE_RECOVERY_MAX_FLIPS) return cost;
            BlockVerdict v;
            v.block.parent   = 0;
            v.block.slot     = __at;
            v.block.expected = MAP_ENTRY_FILE_HEADER;
            v.block.target   = __at;
            v.class_         = RepairClass::RootRepaired;
            v.bit_cost       = cost;
            v.repaired       = __want;
            root_verdicts.push_back(std::move(v));
            return cost;
        };
        const std::uint32_t cost_magic =
            probe_field(b::FILE_HEADER::offset::MAGIC, ::Iris::File::constants::MAGIC_BYTES, 4);
        const std::uint32_t cost_recovery = probe_field(
            b::FILE_HEADER::offset::RECOVERY,
            static_cast<std::uint16_t>(k::RecoveryCodes::RECOVER_FILE_HEADER), 2);
        // FILE_SIZE: known from the OS for a file on disk — the mapping size
        // this engine was constructed with. Not read by validate(), so its
        // damage is benign; it is probed for byte-exactness and because it
        // corroborates the identity gate below.
        probe_field(b::FILE_HEADER::offset::FILE_SIZE, static_cast<std::uint64_t>(m_size), 8);

        // THE IDENTITY GATE. MAGIC and the root RECOVERY tag are the
        // format's identity markers. If BOTH are beyond the flip budget, the
        // bit-flip threat model is not the explanation — these bytes are not
        // a lightly damaged Iris file, and may not be an Iris file at all.
        // Repairing FILE_SIZE/version then would FABRICATE a header on
        // whatever this is: the file would read as valid while being
        // something else. So nothing is written, and the refusal is
        // recorded, never silent. (FILE_SIZE alone is weak corroboration —
        // it must agree with the OS — but it cannot substitute for identity
        // when both markers are gone; neither can version, which any
        // plausible file could carry.)
        if (cost_magic > IFE_RECOVERY_MAX_FLIPS &&
            cost_recovery > IFE_RECOVERY_MAX_FLIPS) {
            failures.push_back({ProducerFailureKind::NotAnIrisFile, 0,
                                MAP_ENTRY_FILE_HEADER,
                                k::RecoveryCodes::RECOVER_UNDEFINED,
                                "magic and root tag both beyond the flip budget — not a lightly damaged Iris file; nothing fabricated"});
            root_verdicts.clear();
        } else {
            // Extension version: the cheapest unique version this reader
            // knows. A version the reader already knows (cost 0 — e.g. a
            // 1.0 file read by a 1.1 reader) is not damage and earns no
            // write, but it does correct `version` when magic damage made
            // declared_version fall back to VERSION_WRITTEN. Equal costs are
            // never guessed; the field stays as read.
            const std::uint32_t written = b::VERSION_WRITTEN;
            const std::uint16_t wmaj = static_cast<std::uint16_t>(written >> 16);
            const std::uint16_t wmin = static_cast<std::uint16_t>(written & 0xFFFFu);
            const std::uint32_t current = p::compose_version(
                ::Iris::File::load<std::uint16_t>(m_base + b::FILE_HEADER::offset::EXTENSION_MAJOR),
                ::Iris::File::load<std::uint16_t>(m_base + b::FILE_HEADER::offset::EXTENSION_MINOR));
            std::uint32_t best = IFE_RECOVERY_MAX_FLIPS + 1;
            std::uint32_t best_v = 0;
            bool unique = true;
            for (std::uint16_t minor = 0; minor <= wmin; ++minor) {
                const std::uint32_t cand = p::compose_version(wmaj, minor);
                const std::uint32_t cost = Recovery::hamming_cost(current, cand);
                if (cost < best) { best = cost; best_v = cand; unique = true; }
                else if (cost == best) unique = false;
            }
            if (best == 0) {
                version = current;   // a known version, intact — but not VERSION_WRITTEN
            } else if (best <= IFE_RECOVERY_MAX_FLIPS && unique) {
                BlockVerdict v;
                v.block.parent   = 0;
                v.block.slot     = b::FILE_HEADER::offset::EXTENSION_MAJOR;
                v.block.expected = MAP_ENTRY_FILE_HEADER;
                v.block.target   = b::FILE_HEADER::offset::EXTENSION_MAJOR;
                v.class_         = RepairClass::RootRepaired;
                v.bit_cost       = best;
                v.repaired       = best_v;
                root_verdicts.push_back(std::move(v));
                version = best_v;   // reconcile under the corrected version
            }
        }
    }
    const bool root_repair = !root_verdicts.empty();

    // THE SCANNED PRODUCER (RC-6 / REC-19.5): the parallel byte census, with
    // its own audit (ScanTagInvalid) merged into the report's failure list.
    FileMap map = scan();
    failures.insert(failures.end(), map.failures.begin(), map.failures.end());

    // THE HIERARCHICAL PRODUCER (RC-6 / REC-19.4): the offset-chain walk with
    // one-surviving-witness descent, audited as it goes, and PARENT-ATTESTED
    // ADMISSION — a block whose VALIDATION was the damaged half is absent
    // from the census, but the parent still names its address and its type,
    // so the walk admits it to the map and its run stops tiling as a hole.
    // The walk used to be gated on root.validate() — MAGIC + RECOVERY — so a
    // damaged header stopped the walk before its own slots could be repaired.
    // A root whose damage is within the probe's budget is walked anyway: the
    // slots are enumerated and classified like any other reference, and the
    // header verdicts below are applied in the same transaction.
    bool map_changed = false;
    if (root.validate() || (root_fits && root_repair))
        map_changed = drive_walk(m_base, m_size, version, visited, refs,
                                 &failures, &map, {{0, MAP_ENTRY_FILE_HEADER}});

    // The orphan census — scan-found blocks no reference names — plus the
    // CENSUS REMAINDER (REC-19): a block the walk never reached is still a
    // real block, and an orphaned parent's outgoing references are still
    // real. Each orphan is enumerated under its own wire type with the same
    // descent rule, audit and admission as the walk, so a parent whose slot
    // took the flip does not take its whole subtree with it.
    std::unordered_set<Offset> named;
    for (const BlockRef& r : refs)
        if (r.target != k::NULL_OFFSET) named.insert(r.target);
    std::unordered_map<uint16_t, std::vector<Offset>> orphans;
    for (const auto& [off, e] : map) {
        if (e.type == MAP_ENTRY_UNDEFINED || e.type == MAP_ENTRY_TILE_FRAME) continue;
        if (!named.count(off)) {
            orphans[static_cast<uint16_t>(e.type)].push_back(off);
            if (!visited.count(off))
                map_changed |= drive_walk(m_base, m_size, version, visited, refs,
                                          &failures, &map, {{off, e.type}});
        }
    }
    // Re-tile when the walk admitted anything: an admitted block's run is now
    // claimed, and a hole that closed must not stay reported. On a clean
    // stream nothing is admitted and this is a no-op.
    if (map_changed) find_gaps(map);

    const auto count = [&report](const BlockVerdict& v) {
        switch (v.class_) {
            case RepairClass::Intact:            ++report.intact; break;
            case RepairClass::Corroborated:      ++report.corroborated; break;
            case RepairClass::TagRepaired:       ++report.tag_repaired; break;
            case RepairClass::PositionRepaired:  ++report.position_repaired; break;
            case RepairClass::ExtentDerived:     ++report.extent_derived; break;
            case RepairClass::Ambiguous:         ++report.ambiguous; break;
            case RepairClass::Unrecovered:       ++report.unrecovered; break;
            case RepairClass::HoleCorroborated:  ++report.hole_corroborated; break;
            case RepairClass::RootRepaired:       ++report.root_repaired; break;
        }
    };
    // REC-20.2 — the ranked hole-candidate pool, built ONCE at the tightest
    // band. classify_one (every call site: the first pass, the reapply, the
    // band loop's follow) matches against this same pool, so every reference
    // is judged by exactly the same rule. `pool` is read-only; the reapply
    // and the band loop admit their winners into `map`, which is why the
    // ranker re-checks map.count(hc.pos) before matching a candidate.
    const std::vector<HoleCandidate> pool = collect_hole_candidates(map, m_base, 2);
    const auto classify_one = [&](const BlockRef& r, BlockVerdict& v) {
        v.block = r;
        classify(v, map, orphans, pool, m_base, version);
    };

    // THE 10-BYTE PAIR, WRITTEN BACK (RC-?): a slot-only repair
    // (Corroborated / HoleCorroborated) rewrites the PARENT's pointer — the
    // first half of the {offset | recovery} pair the match priced. The block
    // the repair points at may still carry its own header damage: the tag
    // that made it invisible to the census (measured: TILE_TABLE 0x5502 ->
    // 0x5402 — the block self-validated, the census recorded ScanTagInvalid,
    // and the read-back walk refused to descend under the broken tag, losing
    // the whole subtree despite Intact verdicts) or a VALIDATION word still
    // short of its own address (a repair can restore the slot to a block
    // whose census entry never existed). Ask the SAME classifier that
    // repaired the edge to audit the block itself: it emits TagRepaired (tag
    // word at off+8) or PositionRepaired (validation word at off), the
    // second half of the pair. TagRepaired's write gate still protects an
    // innocent block — a plausible-but-foreign tag stays unrewritten.
    std::size_t header_audits = 0;
    const auto audit_block_header = [&](Offset __off, MapEntryType __type) {
        if (__off == k::NULL_OFFSET || __type == MAP_ENTRY_TILE_PIXEL_DATA)
            return;
        if (!in_range(__off, p::BlockHeader::HEADER_SIZE, m_size)) return;
        BlockRef ref;
        ref.parent     = 0;
        ref.slot       = 0;
        ref.expected   = __type;
        ref.target     = __off;
        ref.validation = static_cast<Offset>(
            ::Iris::File::load<std::uint64_t>(m_base + __off));
        ref.recovery   = tag_at(m_base, m_size, __off);
        if (ref.validation == __off &&
            ref.recovery != k::RecoveryCodes::RECOVER_UNDEFINED &&
            Abstraction::entry_for(ref.recovery) == __type)
            return;  // the header is already the block it claims to be
        BlockVerdict v;
        classify_one(ref, v);
        // ONLY in-place verdicts are accepted: a repoint verdict on this
        // synthetic ref (parent 0, slot 0) would write the ROOT's first
        // bytes. The block's address is already fixed — the repair wanted
        // here is the damaged half of ITS header, which TagRepaired and
        // PositionRepaired write at off+8 and off respectively.
        if (v.class_ != RepairClass::TagRepaired &&
            v.class_ != RepairClass::PositionRepaired &&
            v.class_ != RepairClass::ExtentDerived)
            return;
        count(v);
        ++header_audits;  // not a reference; blocks_total must not count it
        report.blocks.push_back(std::move(v));
    };

    for (const BlockRef& ref : refs) {
        BlockVerdict verdict;
        classify_one(ref, verdict);
        count(verdict);
        report.blocks.push_back(std::move(verdict));
    }

    // REC-19.7 — REAPPLY: repair → follow the fixed block deeper, now behind
    // the WEAK gate (FastFHIR BatchTest::NoWildPointers — recovery_handoff
    // item 7).
    //
    // A repair restores the child's readability, but the subtree BELOW it was
    // never enumerated under the corrected type: a Corroborated repoint
    // points somewhere the walk never went, a PositionRepaired address was
    // beyond the walk's flip budget. The report used to be incomplete until
    // apply() wrote the repairs back and the stream was re-scanned; this loop
    // closes that gap on the report side.
    //
    // THE GATE IS DELIBERATELY THE WEAK ONE. Verifying the batch with
    // Verifying the batch with every-child-corroborates — every child must
    // carry a surviving witness — is right when the block's type is a HYPOTHESIS and exactly wrong here: a
    // block whose one child is itself damaged has no corroborating child BY
    // DEFINITION, so its batch was discarded and the chain stopped dead at
    // the first repair. Parent broken → child broken → grandchild broken is
    // precisely what this loop exists to unwind (FastFHIR measured: the
    // strong gate left 17 of 24 references with one generation recovered; the
    // weak gate took it to 21 of 24 with two). The type here came from a
    // classified repair — corroborated under the flip budget — so the only
    // question left is whether the read produced wild pointers, and a wrong
    // V-Table's offsets are overwhelmingly outside the stream. `visited`
    // bounds total work — each block is enumerated at most once, so blocks
    // the walk already enumerated are skipped here and cycles are impossible.
    struct ReapplyStep { Offset off; MapEntryType type; std::size_t depth; };
    std::vector<ReapplyStep> repaired;
    for (const BlockVerdict& v : report.blocks) {
        if (v.class_ != RepairClass::Corroborated &&
            v.class_ != RepairClass::HoleCorroborated &&
            v.class_ != RepairClass::TagRepaired &&
            v.class_ != RepairClass::PositionRepaired)
            continue;
        const Offset target = repaired_target(v);
        if (target != k::NULL_OFFSET && !visited.count(target))
            repaired.push_back({target, v.block.expected, 0});
    }
    const auto follow_deep = [&](std::vector<ReapplyStep> queue) {
    while (!queue.empty()) {
        std::vector<ReapplyStep> next;
        Walker w{m_base, m_size, version};
        for (const ReapplyStep& step : queue) {
            if (!visited.insert(step.off).second) continue;
            if (step.depth >= IFE_RECOVERY_MAX_DEPTH) continue;
            // Admit the reconstructed block to the census: a block a repair
            // restored is not recovered until the map says it is there — the
            // tiling is what reports holes, and the hole this repair filled
            // must not stay reported as damage.
            if (!map.count(step.off)) {
                note(map, step.type, step.off,
                     claimable(step.off, extent_of(m_base, m_size, step.off,
                                                   version, step.type), m_size));
                map_changed = true;
            }
            // The block itself, before its children: a slot-only repair may
            // have restored the pointer to a block whose own header (tag or
            // VALIDATION) is still damaged — repair that half of the pair
            // too, or the read-back walk will not descend past it.
            audit_block_header(step.off, step.type);
            std::vector<BlockRef> scratch;
            enumerate_one_level(step.off, step.type, w, scratch);
            // EVERY ref of the batch is reported and processed PER CHILD —
            // IFE's walk refuses to descend into the repaired block (the
            // strict rule), so this is the only pass that enumerates its
            // subtree. The weak gate applies per child, NEVER per batch: one
            // wild sibling (a second flip in one slot names an address near
            // 2^40) must not discard the batch and strand the coherent
            // siblings — measured: a single wild LAYER_EXTENTS slot cost
            // TILE_OFFSETS and its three tile children a batch `continue`
            // (T13: `HoleCorroborated rep 38` + `PositionRepaired @38`, yet
            // 114's edges never enumerated). A wild child classifies
            // Unrecovered below — never guessed, never followed deeper — and
            // its siblings see the same pool and the same rule.
            for (const BlockRef& r : scratch) {
                if (r.target == k::NULL_OFFSET) continue;
                BlockVerdict v;
                classify_one(r, v);
                count(v);
                report.blocks.push_back(std::move(v));
                if (audit_ref(m_size, r, &failures)) {
                    if (v.class_ == RepairClass::Corroborated ||
                        v.class_ == RepairClass::HoleCorroborated ||
                        v.class_ == RepairClass::TagRepaired ||
                        v.class_ == RepairClass::PositionRepaired) {
                        const Offset deeper = repaired_target(report.blocks.back());
                        if (deeper != k::NULL_OFFSET && !visited.count(deeper))
                            next.push_back({deeper, v.block.expected, step.depth + 1});
                    }
                } else if (r.expected != MAP_ENTRY_TILE_PIXEL_DATA &&
                           self_repairable(m_base, m_size, r.target) &&
                           tag_corroborates(m_base, m_size, r.target, r.expected) &&
                           !visited.count(r.target)) {
                    // Coherent under the corrected type — walk deeper, under
                    // the same strict rule as the walk itself.
                    next.push_back({r.target, r.expected, step.depth + 1});
                }
            }
        }
        queue.swap(next);
    }
    };
    // The reapply's initial queue: every first-pass repair, followed deep.
    follow_deep(std::move(repaired));

    // REC-20.5/.6 — PROGRESSIVE BAND EXPANSION, DRIVEN BY THE BROKEN REFS.
    //
    // The signature band has to start tight and cannot stay there. At 2 bits
    // the pool is a clean signal across the arena — and blind to a block
    // whose VALIDATION took 3 or more flips (measured on a 512-flip artifact:
    // 6 of the 8 surviving holes). Widening up front is not the answer: the
    // 3+ band is a flat coincidence floor over twelve thousand hole bytes,
    // and admitting it turned clean verdicts Ambiguous by tying against
    // correct repoints. What makes widening safe is doing it LAST, against a
    // pool earlier rounds have already emptied — eight holes is a different
    // proposition from twelve thousand bytes — and capping the expansion at
    // IFE_RECOVERY_MAX_FLIPS so one number governs the whole engine.
    std::size_t widened_matches = 0;
    for (uint32_t band = 2; band <= IFE_RECOVERY_MAX_FLIPS; ++band) {
        // Any references still looking? If not, no band needs trying.
        bool any_broken = false;
        for (const BlockVerdict& v : report.blocks)
            if ((v.class_ == RepairClass::Unrecovered ||
                 v.class_ == RepairClass::Ambiguous) &&
                v.block.expected != MAP_ENTRY_UNDEFINED &&
                v.block.expected != MAP_ENTRY_TILE_PIXEL_DATA &&  // streams: frame/claim path only
                v.block.target != k::NULL_OFFSET) {
                any_broken = true;
                break;
            }
        if (!any_broken) break;

        std::vector<HoleCandidate> cands = collect_hole_candidates(map, m_base, band);
        if (cands.empty()) continue;

        bool progress = true;
        while (progress) {
            progress = false;
            // Index-based: a matched block is followed immediately and its
            // exposed references are appended; those are themselves
            // candidates for the next sweep of this band.
            for (std::size_t vi = 0; vi < report.blocks.size(); ++vi) {
                BlockVerdict& v = report.blocks[vi];
                if (v.class_ != RepairClass::Unrecovered &&
                    v.class_ != RepairClass::Ambiguous)
                    continue;
                const BlockRef r = v.block;  // by value: blocks may reallocate
                if (r.expected == MAP_ENTRY_UNDEFINED ||
                    r.expected == MAP_ENTRY_TILE_PIXEL_DATA ||
                    r.target == k::NULL_OFFSET)
                    continue;
                // The same metric as the classifier: the offset term is scored
                // against the candidate's EXACT position (noise-free), with
                // the candidate's own residual damage carried as separate
                // evidence and its residual tag compared against the slot's
                // expectation.
                uint32_t best_cost = IFE_RECOVERY_MAX_FLIPS + 1;
                Offset winner = k::NULL_OFFSET;
                bool unique = false;
                const uint16_t declared_code =
                    static_cast<uint16_t>(recovery_for(r.expected));
                for (const HoleCandidate& hc : cands) {
                    if (map.count(hc.pos)) continue;  // claimed by an earlier round
                    const uint32_t cost =
                        Recovery::hamming_cost(static_cast<uint64_t>(r.target),
                                               static_cast<uint64_t>(hc.pos))
                        + hc.self_cost
                        + Recovery::hamming_cost(static_cast<uint64_t>(hc.tag),
                                                 static_cast<uint64_t>(declared_code));
                    if (cost < best_cost) { best_cost = cost; winner = hc.pos; unique = true; }
                    else if (cost == best_cost) unique = false;  // a tie — never guessed
                }
                if (!unique || best_cost > IFE_RECOVERY_MAX_FLIPS || winner == k::NULL_OFFSET)
                    continue;
                // The same coherence gate as the first pass: a widened band
                // must not buy a repoint onto floor junk either.
                if (!winner_reads_sane(m_base, m_size, version, winner, r.expected))
                    continue;

                // ADMIT the reconstructed block: the reference chain, the
                // residual self-offset and the recovery tag now agree again.
                // Sized under the corrected type so the tiling claims its run
                // and the hole stops reporting.
                note(map, r.expected, winner,
                     claimable(winner, extent_of(m_base, m_size, winner, version,
                                                 r.expected), m_size));
                map_changed = true;
                v.class_ = RepairClass::HoleCorroborated;
                v.bit_cost = best_cost;
                v.repaired = winner;
                v.candidates.assign(1, winner);
                ++widened_matches;
                progress = true;

                // FOLLOW THE REPAIRED BLOCK IMMEDIATELY. THIS IS THE DESIGN.
                //
                // The moment a reference is repaired, the block it names is
                // back in the hierarchy chain — and it is not merely
                // reachable, it is now a PARENT. Its own outgoing references
                // have never been seen by anything: the walk could not reach
                // it, the scan could not identify it, so its slots were never
                // enumerated and any damage in them was never counted, let
                // alone repaired. So assess it right here — through the same
                // router as the reapply, behind the same weak gate — and let
                // whatever it finds broken join THIS work list: one repair
                // exposes a parent, that parent exposes its children, and a
                // chain of losses unwinds from a single recovered edge.
                // Deferring this to a later pass would mean matching against
                // a hole set that no longer describes the stream.
                if (visited.insert(winner).second) {
                    audit_block_header(winner, r.expected);
                    Walker w{m_base, m_size, version};
                    std::vector<BlockRef> exposed;
                    enumerate_one_level(winner, r.expected, w, exposed);
                    // Per-child, same as the reapply: one wild sibling must
                    // not discard the batch and strand the coherent ones.
                    for (const BlockRef& child : exposed) {
                        if (child.target == k::NULL_OFFSET) continue;
                        (void)audit_ref(m_size, child, &failures);
                        BlockVerdict cv;
                        classify_one(child, cv);
                        count(cv);
                        report.blocks.push_back(std::move(cv));
                    }
                }
            }
            if (progress) {
                // Re-tile: a hole that held more than one lost block now shows
                // its remainder, and the next sweep of this band sees it.
                find_gaps(map);
                cands = collect_hole_candidates(map, m_base, band);
            }
        }
    }

    // The counters were tallied as verdicts were produced; the band loop may
    // have repaired some of them since, so re-derive them rather than let
    // `unrecovered` keep reporting edges this pass repaired.
    if (widened_matches != 0) {
        report.intact = report.corroborated = report.tag_repaired = 0;
        report.position_repaired = report.extent_derived = 0;
        report.ambiguous = report.unrecovered = report.hole_corroborated = 0;
        for (const BlockVerdict& v : report.blocks) count(v);
    }

    // EXCLUSIVITY — TWO REFERENCES CANNOT OWN ONE CHILD (FastFHIR REC-23.2).
    //
    // Every repoint is picked per-reference and greedily: the ranker asks
    // "which surviving block is cheapest from THIS slot" and never asks
    // whether another slot has a better claim on the same block. So one
    // reference can be handed the child another one needed, and both verdicts
    // read as confident. FastFHIR traced one of its eleven mis-attachments to
    // exactly that contention.
    //
    // A claim is settled evidence or it is a guess. An INTACT edge counts as
    // a claim because both its witnesses agree on the wire; a repoint is one
    // hypothesis about a damaged slot. When a repoint lands on a child that
    // an intact edge already owns, or that another repoint also chose, no
    // local evidence says who is entitled to it — so nobody gets it.
    //
    // Verified against the corpus before it was written: a clean file names
    // every child exactly once. The one duplicate the 1.0 witness carries is
    // the frozen writer-defect slot (METADATA's 1.1 CLINICAL_OFFSET, read on
    // a 1.0 file, holding the address of a real ATTRIBUTES block), and it
    // classifies Unrecovered — so it stakes no claim and cannot demote the
    // legitimate edge to that same block.
    //
    // DEMOTION ONLY. This pass can turn a repair into Ambiguous; it can never
    // create one. It therefore cannot invent recovery — it only declines to
    // guess, which is the trade this engine makes everywhere: a missing
    // reference that is reported is honest, a fabricated one is believed.
    {
        std::unordered_map<Offset, std::size_t> claims;
        for (const BlockVerdict& v : report.blocks) {
            if (v.class_ == RepairClass::Intact && v.block.target != k::NULL_OFFSET)
                ++claims[v.block.target];
            else if ((v.class_ == RepairClass::Corroborated ||
                      v.class_ == RepairClass::HoleCorroborated) &&
                     v.repaired != k::NULL_OFFSET)
                ++claims[v.repaired];
        }
        std::size_t contended = 0;
        for (BlockVerdict& v : report.blocks) {
            if (v.class_ != RepairClass::Corroborated &&
                v.class_ != RepairClass::HoleCorroborated)
                continue;
            if (v.repaired == k::NULL_OFFSET || claims[v.repaired] < 2) continue;
            // Report what it wanted: a driver can see the contention that
            // demoted it, which a bare Ambiguous would hide.
            v.candidates.assign(1, v.repaired);
            v.repaired = k::NULL_OFFSET;
            v.class_   = RepairClass::Ambiguous;
            ++contended;
        }
        if (contended != 0) {
            report.intact = report.corroborated = report.tag_repaired = 0;
            report.position_repaired = report.extent_derived = 0;
            report.ambiguous = report.unrecovered = report.hole_corroborated = 0;
            for (const BlockVerdict& v : report.blocks) count(v);
        }
    }

    report.blocks_total = report.blocks.size() - header_audits;

    // RE-TILE LAST. Every earlier tiling predates the repairs above, so a
    // hole a repair filled would still be reported as one. Re-running the
    // sweep over the final census is what makes `holes` mean "still missing
    // AFTER recovery" instead of "was missing before it ran" — and a hole
    // that held more than one lost block correctly shows its remainder.
    if (map_changed) find_gaps(map);
    report.gaps = std::move(map.gaps);
    // A SWITCH, not an if-chain, so -Wswitch (RC-10.5) makes a new GapClass
    // member a compile error here. An else-if tally silently drops one, which
    // is the shape of the bug FastFHIR shipped: the class was assigned by
    // find_gaps() and counted by nobody, so the report understated what was
    // missing.
    for (const Gap& g : report.gaps) {
        switch (g.class_) {
            case GapClass::Hole:        ++report.holes;        break;
            case GapClass::VersionSkew: ++report.version_skew; break;
            case GapClass::Trailing:    break;  // arena slack — not missing
        }
    }
    // The root verdicts are appended last: they name no children, so no pass
    // above may treat them as parents; the header they repair is what makes
    // every other repair reachable on read-back, and apply() writes them in
    // the same all-or-nothing transaction.
    for (BlockVerdict& v : root_verdicts) {
        count(v);
        report.blocks.push_back(std::move(v));
    }
    report.failures = std::move(failures);
    return report;
}

namespace {

/// The one little-endian byte run a verdict implies, resolved (not written)
/// by plan_repair(). apply() collects every plan first — the all-or-nothing
/// range check — then commits them, then verifies each committed run and
/// rolls the WHOLE report back if any fails to verify. The before-images play
/// the role FastFHIR REC-15 gives its working copy: IFE's apply() repairs the
/// caller's own mutable buffer (there is no copy to hand back), so an
/// in-place engine buys the same all-or-nothing guarantee with snapshots.
struct PendingWrite {
    Offset   at     = k::NULL_OFFSET;
    Size     width  = 0;   ///< 2 (tag), 4 (COUNT), 5 (u40 tile offset), or 8
    uint64_t before = 0;   ///< little-endian value currently at `at`
    uint64_t after  = 0;   ///< little-endian value the repair writes
};

/// What a verdict implies for apply(): Nothing — a report, not a repair
/// (Intact / Ambiguous / Unrecovered); Declined — a repair that must not be
/// written (the TagRepaired gate); Write — one byte run to change; or Fail —
/// the repair cannot be honoured, and apply() writes nothing at all.
enum class RepairPlan : uint8_t { Nothing, Declined, Write, Fail };

[[nodiscard]] constexpr uint64_t load_le(const BYTE* __p, Size __width) noexcept {
    uint64_t v = 0;
    for (Size i = 0; i < __width; ++i)
        v |= static_cast<uint64_t>(__p[i]) << (8u * i);
    return v;
}

constexpr void store_le(BYTE* __p, Size __width, uint64_t __v) noexcept {
    for (Size i = 0; i < __width; ++i)
        __p[i] = static_cast<BYTE>(__v >> (8u * i));
}

RepairPlan plan_repair(const Abstraction::BlockVerdict& __v, const BYTE* __base,
                       Size __size, PendingWrite& __pw) noexcept {
    const Abstraction::BlockRef& r = __v.block;
    const auto plan = [&](Offset __at, Size __width, uint64_t __after) {
        __pw.at = __at;
        __pw.width = __width;
        __pw.before = load_le(__base + __at, __width);
        __pw.after = __after;
    };

    switch (__v.class_) {
        case Abstraction::RepairClass::Intact:
        case Abstraction::RepairClass::Ambiguous:
        case Abstraction::RepairClass::Unrecovered:
            return RepairPlan::Nothing;  // reported, never written

        case Abstraction::RepairClass::PositionRepaired: {
            // The child's VALIDATION was the damaged half. A block's
            // self-offset IS its own address, so the corrected value needs no
            // candidate — it is the address the parent named.
            if (!in_range(r.target, 8, __size)) return RepairPlan::Fail;
            plan(r.target, 8, static_cast<uint64_t>(r.target));
            return RepairPlan::Write;
        }

        case Abstraction::RepairClass::TagRepaired: {
            // THE WRITE GATE (FastFHIR REC-15 — recovery_handoff item 18).
            // TagRepaired is decided on "the child validates but its tag
            // disagrees", which is two situations wearing one face: the
            // child's TAG was flipped, or the PARENT's offset was flipped
            // onto an innocent, perfectly valid block of another type. The
            // ranker picks whichever is cheaper in bits and is sometimes
            // wrong — which costs nothing while merely READING (one
            // traversal mis-types one block), but is destructive while
            // WRITING: relabelling an innocent block erases the only
            // surviving record of what it really was, and its own
            // references stop being enumerable at all. Measured in
            // FastFHIR, applying all 61 tag rewrites on a 512-flip artifact
            // bought +10 intact edges and created 59 unrecovered ones and 7
            // new holes — strictly worse than not repairing. A tag that is
            // NOT a plausible type cannot be an innocent block's real type,
            // so there is nothing to destroy: that is the only case this
            // writes. A plausible-but-different tag stays a report.
            if (!in_range(r.target, p::BlockHeader::HEADER_SIZE, __size))
                return RepairPlan::Fail;
            const k::RecoveryCodes wire = static_cast<k::RecoveryCodes>(
                ::Iris::File::load<std::uint16_t>(__base + r.target + p::BlockHeader::RECOVERY));
            // ADJUDICATED (RC-8 / REC-22.2). When consensus named the child's
            // tag the damaged half, the "innocent block" worry above is
            // answered by evidence — the child's slots read coherently under
            // the slot's declared type — rather than by a plausibility guess,
            // and the rewrite may land even though the wire tag is plausible.
            if (!__v.tag_adjudicated && wire != k::RecoveryCodes::RECOVER_UNDEFINED &&
                Abstraction::entry_for(wire) != Abstraction::MAP_ENTRY_UNDEFINED)
                return RepairPlan::Declined;
            plan(r.target + p::BlockHeader::RECOVERY, 2,
                 static_cast<uint16_t>(Abstraction::recovery_for(r.expected)));
            return RepairPlan::Write;
        }

        case Abstraction::RepairClass::Corroborated:
        case Abstraction::RepairClass::HoleCorroborated: {
            // The parent's stored offset was the damaged half; the candidate
            // the ranker chose is the child.
            if (__v.repaired == k::NULL_OFFSET) return RepairPlan::Fail;
            // The one non-u64 slot in the edge inventory (RC-1.1): a tile
            // entry's OFFSET is a 40-bit self-offset, and writing a full u64
            // here would clobber the adjacent u24 SIZE field.
            const Size width = r.expected == Abstraction::MAP_ENTRY_TILE_PIXEL_DATA
                             ? b::TILE_OFFSETS::TILE_OFFSET::size::OFFSET : 8;
            if (!in_range(r.parent, r.slot, __size)
                || !in_range(r.parent + r.slot, width, __size)) return RepairPlan::Fail;
            plan(r.parent + r.slot, width, static_cast<uint64_t>(__v.repaired));
            return RepairPlan::Write;
        }

        case Abstraction::RepairClass::ExtentDerived: {
            // The array's stamped ENTRY_COUNT disagreed with the tiling; the
            // report carries the recomputed extent, so the COUNT is re-derived
            // from it and the on-wire STRIDE.
            if (!in_range(r.target, p::ArrayHeader::HEADER_SIZE, __size))
                return RepairPlan::Fail;
            const std::uint16_t stride = ::Iris::File::load<std::uint16_t>(
                __base + r.target + p::ArrayHeader::STRIDE);
            if (stride == 0 || __v.repaired < p::ArrayHeader::HEADER_SIZE)
                return RepairPlan::Fail;
            const std::uint32_t count = static_cast<std::uint32_t>(
                (static_cast<uint64_t>(__v.repaired) - p::ArrayHeader::HEADER_SIZE)
                / stride);
            plan(r.target + p::ArrayHeader::COUNT, 4, count);
            return RepairPlan::Write;
        }

        case Abstraction::RepairClass::RootRepaired: {
            // A FILE_HEADER field rewritten to a value the format itself
            // defines — the strongest evidence in the engine, so no
            // plausibility gate applies (there is nothing to be innocent
            // about: the root is a constant shape, not a candidate).
            // `repaired` carries the known value; the width comes from the
            // fixed root layout (MAGIC and the extension version are u32s at
            // 0 and 14; RECOVERY is the root's u16 type word at 4) so a
            // corrupt verdict cannot choose its own write.
            const Size width = r.target == b::FILE_HEADER::offset::RECOVERY
                             ? 2 : 4;
            if (__v.repaired == k::NULL_OFFSET ||
                !in_range(r.target, width, __size))
                return RepairPlan::Fail;
            plan(r.target, width, static_cast<std::uint64_t>(__v.repaired));
            return RepairPlan::Write;
        }
    }
    return RepairPlan::Fail;
}

}  // namespace

bool Recovery::apply(const Abstraction::RecoveryReport& __report) noexcept {
    // apply() is the only mutating path; the mapping must have been opened
    // writable for it. NOTHING IS WRITTEN until every repair in the report
    // has been planned and range-checked — a half-repaired file is worse than
    // an untouched one, and an engine that writes as it goes cannot honour
    // that no matter what its comments say. (It did, until 2026-08-28: the
    // refusal arrived after the earlier writes had already landed.)
    //
    // Every write is then verified by re-reading the run it changed, and one
    // that does not verify rolls the WHOLE report back to its before-images
    // (FastFHIR REC-15: write, re-verify, revert — ported RC-7; the damaged
    // original must stay readable for a before/after comparison, so an
    // in-place engine keeps it that way by restoring it on failure).
    // Ambiguous and Unrecovered are never written even when a filter asks for
    // them — the engine reported those because it declined to choose, and
    // writing a guess converts a declared uncertainty into a silent one.
    BYTE* const base = const_cast<BYTE*>(m_base);
    std::vector<PendingWrite> writes;
    writes.reserve(__report.blocks.size());
    for (const Abstraction::BlockVerdict& v : __report.blocks) {
        PendingWrite pw;
        switch (plan_repair(v, m_base, m_size, pw)) {
            case RepairPlan::Fail: return false;  // nothing is applied
            case RepairPlan::Write: writes.push_back(pw); break;
            case RepairPlan::Nothing:
            case RepairPlan::Declined: break;  // reported, not written
        }
    }
    for (const PendingWrite& w : writes)
        store_le(base + w.at, w.width, w.after);
    for (const PendingWrite& w : writes) {
        if (load_le(base + w.at, w.width) == w.after) continue;
        // REVERT. A write that does not verify is not a repair, and leaving
        // it in place would make the buffer worse than the damaged original
        // while reporting success — the one outcome this must never produce.
        for (auto it = writes.rbegin(); it != writes.rend(); ++it)
            store_le(base + it->at, it->width, it->before);
        return false;
    }
    return true;
}

std::uint32_t Recovery::hamming_cost(std::uint64_t __a, std::uint64_t __b) noexcept {
    return static_cast<std::uint32_t>(std::popcount(__a ^ __b));
}

}  // namespace Iris::File
