/**
 * @file IFE_Advanced.hpp
 * @brief Iris File Extension — the ADVANCED tier: recover and modify a file.
 *
 * TIERS. IFE's public surface is split three ways, and the split is the point
 * (the same shape FastFHIR uses — see ../FastFHIR/include/FF_Handles.hpp and
 * architecture.md §2.5):
 *
 *   1. PUBLIC   — `IrisFileExtension.hpp`. Reading a file: the entry points
 *                 (`is_iris_codec_file`, `validate_file_structure`,
 *                 `abstract_file_structure`) and the `Abstraction::*` read
 *                 types. Nothing below this line is needed to open a slide.
 *   2. ADVANCED — this header. Recovering or modifying a file: the file map
 *                 (`FileMap`, `FileMapEntry`, `MapEntryType`), the gap census,
 *                 the per-reference verdicts (`BlockRef`, `BlockVerdict`,
 *                 `RepairClass`), and the two entry points that produce them
 *                 (`generate_file_map`, `recover_file_structure`). The public
 *                 header's own documentation already says generate_file_map
 *                 "is not a cheap method and does not need to be routinely
 *                 done; only when recovering or modifying a file" — this
 *                 header is where that sentence becomes structural.
 *   3. INTERNAL — `IFE_Primitives.hpp`, `IFE_Bytes.hpp`, and generated_source/.
 *                 Block layout and byte arithmetic. Not part of the consumer
 *                 surface; reached through the generated handles.
 *
 * WHY THE SPLIT. A consumer that only reads a slide (`Iris-Codec`) should not
 * see the recovery apparatus in its include graph, its autocomplete, or its
 * ABI. Including `IrisFileExtension.hpp` alone now gives exactly the read
 * surface; a caller that recovers includes this header instead. Reading is the
 * default; recovery is opt-in.
 *
 * DEPENDENCY NOTE. This header is included by `IFE_Primitives.hpp`, whose
 * `note()` helper records into `FileMap`. The primitives layer already depends
 * on the semantic layer (it includes `IrisFileExtension.hpp`), so that edge is
 * deliberate, not inverted layering.
 *
 * @copyright Iris Developers, 2025-2026
 */

#ifndef IFE_Advanced_hpp
#define IFE_Advanced_hpp

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "IrisFileExtension.hpp"

namespace Iris::File {
namespace Abstraction {

/// Which kind of block a file-map entry describes.
enum IFE_EXPORT MapEntryType {
    MAP_ENTRY_UNDEFINED = 0,
    MAP_ENTRY_FILE_HEADER,
    MAP_ENTRY_TILE_TABLE,
    MAP_ENTRY_CIPHER,
    MAP_ENTRY_METADATA,
    MAP_ENTRY_ATTRIBUTES,
    MAP_ENTRY_LAYER_EXTENTS,
    MAP_ENTRY_TILE_PIXEL_DATA,
    MAP_ENTRY_TILE_OFFSETS,
    MAP_ENTRY_ATTRIBUTE_SIZES,
    MAP_ENTRY_ATTRIBUTE_BYTES,
    MAP_ENTRY_IMAGES,
    MAP_ENTRY_IMAGE_BYTES,
    MAP_ENTRY_ICC_PROFILE,
    MAP_ENTRY_ANNOTATIONS,
    MAP_ENTRY_ANNOTATION_BYTES,
    MAP_ENTRY_ANNOTATION_GROUP_SIZES,
    MAP_ENTRY_ANNOTATION_GROUP_BYTES,
    MAP_ENTRY_CLINICAL_METADATA,
    MAP_ENTRY_TILE_FRAME,
};

/// Why a run of bytes belongs to no file-map entry (MIGRATION.md RC-2.3).
enum class IFE_EXPORT GapClass : uint8_t {
    Hole = 0,        ///< unattributed, and large enough to have been a block.
                     ///< A block whose VALIDATION AND parent reference are
                     ///< both broken has NO surviving witness — absence is the
                     ///< only evidence it existed.
    VersionSkew,     ///< benign: a newer engine appended fields, so THIS
                     ///< reader under-sizes every block of that tag and each
                     ///< trails a constant run. Never damage.
    Trailing,        ///< after the last entry, before EOF.
};

/// One unattributed run in the arena tiling (REC-18, ported as RC-2).
struct IFE_EXPORT Gap {
    Offset   start  = ::Iris::File::constants::NULL_OFFSET;
    Size     length = 0;
    GapClass class_ = GapClass::Hole;
};

/**
 * @brief A datablock within the IFE file structure system.
 *
 * The entry carries the offset and the type; a caller builds the handle it
 * wants — a generated handle is constructed from an offset, not downcast from
 * a base:
 *
 * ```cpp
 * case MAP_ENTRY_TILE_TABLE: {
 *     Iris::File::blocks::TILE_TABLE table {base, entry.offset, file_size, version};
 *     if (table.validate()) ... // read through the handle
 * }
 * ```
 */
struct IFE_EXPORT FileMapEntry {
    MapEntryType type   = MAP_ENTRY_UNDEFINED;
    Offset       offset = ::Iris::File::constants::NULL_OFFSET;
    Size         size   = 0;
};

/// Why the recovery audit recorded a reference (FastFHIR REC-19.2, ported as
/// RC-6). The audit is the report-side guarantee that damage is never silent:
/// before RC-6 a block whose VALIDATION was the damaged half vanished from
/// the scan census, its subtree's references were never enumerated, and the
/// report showed zero failures — a loss that could not be seen in the report
/// it was absent from. Every damaged reference the walk or the scan sees is
/// recorded here, typed, with the offending values.
enum class IFE_EXPORT ProducerFailureKind : uint8_t {
    ScanTagInvalid,         ///< census: self-offset consistent, but the wire
                            ///< recovery tag is not a known type
    VTableRecoveryMismatch, ///< walk: the child self-validates, but its wire
                            ///< tag contradicts the slot's expected type
    InvalidSelfRef,         ///< walk: the slot names a block whose VALIDATION
                            ///< is not its own address
    /// Root probe: MAGIC and the root RECOVERY tag are BOTH beyond the flip
    /// budget. The header's identity is gone — these bytes are not a lightly
    /// damaged Iris file (they may not be an Iris file at all) — so the
    /// probe writes NOTHING. A fabricated header on random data would read
    /// as valid while being something else, which is the one outcome the
    /// engine exists to prevent.
    NotAnIrisFile,
};

/// One typed audit record (FastFHIR ProducerFailure).
struct IFE_EXPORT ProducerFailure {
    ProducerFailureKind kind     = ProducerFailureKind::InvalidSelfRef;
    Offset              at       = ::Iris::File::constants::NULL_OFFSET;  ///< the damaged offset
    MapEntryType        expected = MAP_ENTRY_UNDEFINED;  ///< the slot's compiled expectation (1c)
    /// The wire recovery tag read at `at`; RECOVER_UNDEFINED when none could
    /// be read or none was expected.
    ::Iris::File::constants::RecoveryCodes actual = ::Iris::File::constants::RecoveryCodes::RECOVER_UNDEFINED;
    const char* why = nullptr;  ///< invariant-style one-liner; never null
};

struct IFE_EXPORT FileMap : public std::map<Offset, FileMapEntry> {
    Size file_size = 0;
    /// Runs of bytes no entry claims, populated by Recovery::scan() (RC-2.3).
    /// A non-empty vector here means the file has holes, skew, or trailing
    /// slack — the caller reads `class_` to tell damage from benign runs.
    std::vector<Gap> gaps;
    /// The census's own audit (RC-6): self-consistent offsets whose recovery
    /// tag is not a known type. The entry is NOT recorded — an unknown type
    /// has no extent to tile with — but the fact is never dropped. recover()
    /// merges these into the report's failure list.
    std::vector<ProducerFailure> failures;
};

/// One parent→child block reference, with both wire witnesses and the
/// compiled expectation (FastFHIR BlockRef, P0-3). The atom recovery counts.
struct IFE_EXPORT BlockRef {
    Offset       parent    = ::Iris::File::constants::NULL_OFFSET; ///< 1a: the block owning the slot
    Size         slot      = 0;               ///< 1a: byte offset of the slot within parent
    MapEntryType expected  = MAP_ENTRY_UNDEFINED;  ///< 1c: what the slot must point at — compiled in
    Offset       target    = ::Iris::File::constants::NULL_OFFSET;  ///< 1b: the offset stored in the slot
    Offset       validation = ::Iris::File::constants::NULL_OFFSET; ///< 2a: VALIDATION read at target
    /// 2b: the RECOVERY tag read at target. For a TILE_PIXEL_DATA edge this
    /// carries no tag (streams are unframed); the frame's TILE_INDEX, or
    /// RECOVER_UNDEFINED when no frame precedes the stream.
    ::Iris::File::constants::RecoveryCodes recovery = ::Iris::File::constants::RecoveryCodes::RECOVER_UNDEFINED;
    /// The claimed extent of the child, from the parent's own bytes — for a
    /// TILE_PIXEL_DATA edge, the entry's SIZE field (the stream's only extent
    /// witness). Zero when the parent carries no size claim. The tile
    /// reconciliation ranks with it: a candidate whose claimed run would not
    /// tile the arena (overlap or gap) is not the child (RC-3.4).
    Size         extent    = 0;
};

/// How a damaged reference was repaired, or why it was not (RC-1).
enum class IFE_EXPORT RepairClass : uint8_t {
    Intact = 0,        ///< both witnesses agreed on the wire — nothing to repair
    Corroborated,      ///< parent slot corrupt; a unique matching orphan names the child
    TagRepaired,       ///< child RECOVERY rewritten from the parent's expectation
    PositionRepaired,  ///< child VALIDATION recomputed from the parent-named address
    ExtentDerived,     ///< array COUNT/STRIDE corrupt; extent recomputed
    Ambiguous,         ///< ≥2 live readings at equal cost — reported, never guessed
    Unrecovered,       ///< no candidate within the flip budget
    /// Both witnesses gone and no orphan within budget, but a HOLE the tiling
    /// located sits where the child was (RC-2.4). The repair restores the
    /// parent's pointer, NOT the child: the bytes at that address are the
    /// destroyed ones, and a reader following the repaired slot will fail
    /// validate() there. Strictly weaker evidence than Corroborated — a
    /// surviving block says "I am here", a hole says only "something of this
    /// size was here" — and counted apart for that reason.
    HoleCorroborated,
    /// The FILE_HEADER itself was the damaged half. The root is the one
    /// structure with no wire witness AND no VALIDATION, but its shape is
    /// fully known before it is read: MAGIC and RECOVERY are constants the
    /// format defines, the extension version must be one this reader knows,
    /// and the FILE_SIZE field must equal the mapping's size. Those
    /// self-constraints are the witness — the header is recovered by virtue
    /// of being a header, and a repair writes a known constant, never a
    /// candidate (strictly stronger evidence than either ranked hypothesis).
    RootRepaired,
};

/// One block reference plus its verdict.
struct IFE_EXPORT BlockVerdict {
    BlockRef            block;
    RepairClass         class_    = RepairClass::Unrecovered;
    std::uint32_t       bit_cost  = 0;   ///< Hamming cost of the repair, 0 = intact
    std::vector<Offset> candidates;      ///< populated for Ambiguous
    /// The offset apply() writes: for Corroborated the chosen child to store
    /// into the parent slot; for ExtentDerived the recomputed array extent.
    /// NULL_OFFSET when the repair writes nothing.
    Offset              repaired  = ::Iris::File::constants::NULL_OFFSET;
    /// TagRepaired only (RC-8 / FastFHIR REC-22.2): the rewrite was decided by
    /// COHERENCE, not cost — the child's own slots read coherently under the
    /// slot's declared type (block_reads_as), so the wire tag is the damaged
    /// half even though it is a plausible type. apply() may then rewrite a
    /// plausible tag; without this flag it declines to, because a plausible
    /// tag could be an innocent block's real type.
    bool                tag_adjudicated = false;
};

/// The RC-1 reconciliation result. Counts are precomputed so a driver can
/// report "blocks recovered / total blocks" without re-walking the vectors.
struct IFE_EXPORT RecoveryReport {
    std::size_t blocks_total       = 0;
    std::size_t intact             = 0;
    std::size_t corroborated       = 0;
    std::size_t tag_repaired       = 0;
    std::size_t position_repaired  = 0;
    std::size_t extent_derived     = 0;
    std::size_t ambiguous          = 0;
    std::size_t unrecovered        = 0;
    /// Repairs made against a hole rather than a surviving block (RC-2.4).
    /// Counted apart from `corroborated` because the evidence is weaker: the
    /// pointer is restored, the bytes it names are still destroyed.
    std::size_t hole_corroborated  = 0;
    /// FILE_HEADER fields rewritten from the format's own constants (MAGIC,
    /// RECOVERY, extension version). Counted apart: no reference was broken,
    /// but without the root no reference is reachable.
    std::size_t root_repaired      = 0;

    /// Every enumerated block reference, each with its verdict.
    std::vector<BlockVerdict> blocks;

    /// The gap census (RC-2): `holes` is the count that means damage — a
    /// block whose VALIDATION AND parent reference are broken leaves no
    /// witness at all. Version skew and trailing slack are counted apart
    /// because neither is damage.
    std::size_t      holes        = 0;
    std::size_t      version_skew = 0;
    std::vector<Gap> gaps;

    /// The merged producer audit (RC-6 / FastFHIR REC-19.2): every damaged
    /// reference the hierarchical walk, the orphan pass, the reapply, or the
    /// scan census saw, typed. NOT empty on a damaged file whose verdicts all
    /// came out Intact — that combination is how a silently-lost subtree used
    /// to hide, and the whole-report tests assert the audit catches it.
    std::vector<ProducerFailure> failures;
};

}  // namespace Abstraction

// MARK: - ADVANCED ENTRY METHODS

/**
 * @brief Generate a file map showing the offset locations of header and array blocks with their respective
 * types and sizes detailed. This is not a cheap method and does not need to be routinely done; only when
 * recovering or modifying a file.
 *
 * File mapping is an extremely valuable tool for performing file updates to avoid overwriting important data.
 * Fortunately it is very simple to do. Before writing, perform the \ref FileMap::upper_bound (Offset write_offset) method
 * to identify what data exists after your proposed write location. These data will need to be rewritten or,
 * alternatively, shifted and all references to them and their validations updated as well. For this reason, it's usually
 * easier to simply read them into memory and then rewrite them back to disk following the update.
 */
// ALWAYS CREATE A FILE MAP BEFORE PERFORMING AN UPDATE TO A FILE
Abstraction::FileMap IFE_EXPORT generate_file_map       (const FileAccessInfo&);

/**
 * @brief Recover the block structure of a damaged file by scanning for block signatures.
 *
 * New in the generated layer. Where generate_file_map walks the offset graph — and therefore
 * finds nothing below a corrupted pointer — this ignores the graph entirely and scans for the
 * two signatures a block can carry.
 *
 * Most blocks carry a u64 equal to their own offset followed by a u16 in the recovery-tag set;
 * the 0x55 high byte those tags share is what keeps that scan's false-positive rate negligible.
 * A tile frame carries no tag at all, and is instead identified by a *forty*-bit value equal to
 * its own position, which nothing else in the format writes. Frames are reported as
 * MAP_ENTRY_TILE_FRAME alongside the MAP_ENTRY_TILE_PIXEL_DATA stream each one describes.
 *
 * A frame supplies the part of a tile offsets entry that reading the slide cannot: its global
 * tile index. Position cannot supply it, because streams may be written in any order. The
 * stream's *length* is not in the frame and is not reported — that is a question its codec
 * answers, and this layer knows nothing about codecs. FileMapEntry carries no index field, so as
 * with every other type a caller builds the handle and asks it:
 *
 * ```cpp
 * case MAP_ENTRY_TILE_FRAME: {
 *     const auto stream_at = entry.offset + entry.size;   // the frame ends where the stream starts
 *     Iris::File::blocks::TILE_PIXEL_DATA frame {base, stream_at, file_size, version};
 *     if (frame.validate()) rebuilt[*frame.tile_index()] = stream_at;
 * }
 * ```
 *
 * The FILE_HEADER is not recoverable this way and is not reported: it is the one block with no
 * VALIDATION field, because it lives at byte 0 where that field could only ever store zero.
 */
Abstraction::FileMap IFE_EXPORT recover_file_structure  (const FileAccessInfo&);

}  // namespace Iris::File

// The generated wire-tag → map-entry vocabulary (spec-derived, emitted by
// generator/ into generated_source/IFE_Map.hpp). It names MapEntryType, so it
// is included here after the Abstraction namespace closes; it includes nothing
// itself and assumes IrisFileExtension.hpp was included first.
#include "IFE_Map.hpp"

// =====================================================================
// GLOBAL C-STYLE ALIASES — the ADVANCED tier
// =====================================================================
// Mirrors the read-tier aliases in IrisFileExtension.hpp (and FastFHIR's FF_
// block): inside the namespace a type carries its plain C++ name, `IFE_` is
// its global spelling. Aliases only — nothing here renames a namespaced
// symbol.
using IFE_MapEntryType        = Iris::File::Abstraction::MapEntryType;
using IFE_FileMapEntry        = Iris::File::Abstraction::FileMapEntry;
using IFE_FileMap             = Iris::File::Abstraction::FileMap;
using IFE_Gap                 = Iris::File::Abstraction::Gap;
using IFE_GapClass            = Iris::File::Abstraction::GapClass;
using IFE_ProducerFailure     = Iris::File::Abstraction::ProducerFailure;
using IFE_ProducerFailureKind = Iris::File::Abstraction::ProducerFailureKind;
using IFE_BlockRef            = Iris::File::Abstraction::BlockRef;
using IFE_BlockVerdict        = Iris::File::Abstraction::BlockVerdict;
using IFE_RepairClass         = Iris::File::Abstraction::RepairClass;
using IFE_RecoveryReport      = Iris::File::Abstraction::RecoveryReport;

#endif  // IFE_Advanced_hpp
