/**
 * @file IFE_Recovery.hpp
 * @brief Iris File Extension recovery: the census (MIGRATION.md RB-3).
 * @copyright Iris Developers, 2025-2026
 *
 * This header also carries the file map the census works over — the
 * offset-graph types (`FileMap`, `MapEntryType`, `Gap`, `ProducerFailure`) and
 * the graph walk (`generate_file_map`). Those are recovery's substrate, not a
 * separate "advanced" tier, so the recovery surface is ONE header.
 *
 * The engine is FastFHIR's (`../FastFHIR/src/FF_Recovery.cpp`,
 * `recovery_algorithm_handoff.md`): a byte census first, then cycles that
 * decide whole branches. This revision holds the census.
 *
 * Every parent→child reference is witnessed more than once: the parent's word,
 * the child's self-offset (VALIDATION), the child's tag, and the type the
 * parent's field declares, which is compiled in and cannot be damaged. The
 * census reads the bytes once and records what holds and what does not:
 *
 *   scan()     every position holding its own offset (a block), the file header,
 *              and the tile streams and frames the surviving tile entries name;
 *   find_gaps  every run of bytes nothing covers: a Hole, benign VersionSkew,
 *              or Trailing slack;
 *   census()   the scan's blocks as anchors, each hole chained from where the
 *              block before it ended, every slot judged, a link whose witnesses
 *              all hold kept as an edge, the blocks those edges join as islands,
 *              and a question opened for every slot the header's island reaches
 *              and no edge explains.
 *
 * THREAT MODEL: bit flips. Nothing is inserted, deleted or moved.
 */
#ifndef IFE_Recovery_hpp
#define IFE_Recovery_hpp

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "IrisFileExtension.hpp"

namespace Iris::File {

// ---- The file map: the offset-graph model the census reads ---------------- //
// (Folded from the former IFE_Advanced.hpp. The file map, the gap census and
// the producer-failure records are not a separate "modify a file" tier — they
// are the recovery engine's own substrate, so they live with it.)

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
    /// The tag of the entry the run trails (FastFHIR's Gap::after):
    /// RECOVER_UNDEFINED after a tile stream or frame, or at the file's start.
    ::Iris::File::constants::RecoveryCodes after = ::Iris::File::constants::RecoveryCodes::RECOVER_UNDEFINED;
    GapClass    class_ = GapClass::Hole;
    const char* why    = "";
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

/// One field of a block that names a child block: FastFHIR's FF_FieldInfo,
/// reference fields only. reference_fields_view() (generated from the spec's
/// `points_to`, in IFE_Map.hpp) lists them per block type. Two references are
/// not declared that way and are read by hand: a tile entry's u40 OFFSET (a
/// headerless stream) and a nested attribute value (u64 offsets inside
/// ATTRIBUTE_BYTES, sliced by ATTRIBUTE_SIZES).
struct IFE_EXPORT FieldInfo {
    const char*   name         = nullptr;
    /// From the block's start. An entry field (`in_entry`) names entry 0's
    /// copy; entry i's is i × STRIDE further on, STRIDE read from the array.
    std::uint16_t field_offset = 0;
    /// The tag the child must carry: the witness that cannot be damaged,
    /// because it is compiled in rather than read from the file.
    ::Iris::File::constants::RecoveryCodes child_recovery =
        ::Iris::File::constants::RecoveryCodes::RECOVER_UNDEFINED;
    bool          nullable     = false;  ///< NULL_OFFSET is a legal value
    bool          in_entry     = false;  ///< one copy per array entry
    /// The version that added the field (compose_version), or 0 for a 1.0
    /// field, which every file has: gated exactly as the generated accessors are.
    std::uint32_t since        = 0;
};

struct IFE_EXPORT FileMap : public std::map<Offset, FileMapEntry> {
    Size file_size = 0;
    /// Runs of bytes no entry claims (RC-2.3). A non-empty vector here means
    /// the file has holes, skew, or trailing slack — the caller reads `class_`
    /// to tell damage from benign runs. Filled by Recovery::scan();
    /// generate_file_map walks the offset graph only and leaves it EMPTY, so
    /// on its map empty does not mean "no holes".
    std::vector<Gap> gaps;
    /// The census's own audit (RC-6): self-consistent offsets whose recovery
    /// tag is not a known type. The entry is NOT recorded — an unknown type
    /// has no extent to tile with — but the fact is never dropped. Filled by
    /// Recovery::scan(), like `gaps`.
    std::vector<ProducerFailure> failures;
};

}  // namespace Abstraction

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

class IFE_EXPORT Recovery {
public:
    /// Reads nothing at construction. Every later read is bounded by the file.
    explicit Recovery(const FileAccessInfo& info) noexcept;

    /// The byte census: every position whose eight bytes hold its own offset,
    /// sized under the tag it carries; the file header; and every tile stream
    /// (and its frame) that a self-validating tile offsets array names. A
    /// self-validating position whose tag is no type this build knows is not
    /// recorded, and is reported on `failures`. Gaps are filled by find_gaps().
    Abstraction::FileMap scan() const;

    /// Tile `map` and record every run of bytes no entry covers.
    void find_gaps(Abstraction::FileMap& map) const;

    /// How a slot's word names its child.
    enum class SlotRepr : std::uint8_t {
        Absolute,   ///< a u64 offset field (reference_fields_view)
        TileEntry,  ///< a tile offsets entry: u40 OFFSET, u24 SIZE, naming a headerless stream
        Nested,     ///< a u64 offset inside a nested attribute value
    };
    /// One place where a parent names, or may name, a child.
    struct Slot {
        Offset        parent = constants::NULL_OFFSET;  ///< the block holding the word; 0 for the FILE_HEADER
        Offset        seat   = constants::NULL_OFFSET;  ///< absolute position of the word's first byte
        SlotRepr      repr   = SlotRepr::Absolute;
        std::uint64_t stored = 0;                       ///< the word as it stands
        /// The compiled child type. RECOVER_UNDEFINED for a tile entry: a
        /// stream carries no tag.
        constants::RecoveryCodes expect = constants::RecoveryCodes::RECOVER_UNDEFINED;
        bool          nullable = false;                 ///< NULL_OFFSET (NULL_TILE) is legal
        std::uint32_t index    = 0;                     ///< TileEntry: the global tile index
        std::uint32_t claim    = 0;                     ///< TileEntry: the SIZE the entry claims
    };
    /// A link whose every witness holds.
    struct Edge {
        Slot   slot;
        Offset child = constants::NULL_OFFSET;
    };
    /// Blocks joined by such links. The FILE_HEADER's island is attached;
    /// every other island is an orphaned subtree.
    struct Island {
        Offset              root     = constants::NULL_OFFSET;  ///< 0 for the header's island
        bool                attached = false;
        std::vector<Offset> members;                             ///< the root first, then depth first
    };
    enum class PointKind : std::uint8_t {
        Open,         ///< a reachable slot whose child is not established
        ArrayExtent,  ///< a reachable array whose stamped geometry the file cannot hold
    };
    struct Point {
        PointKind kind  = PointKind::Open;
        Slot      slot;                               ///< Open
        Offset    array = constants::NULL_OFFSET;     ///< ArrayExtent
    };
    struct Census {
        Size                          extent  = 0;
        std::size_t                   anchors = 0;  ///< self-validating blocks, the header excluded
        std::vector<Edge>             edges;        ///< every link whose witnesses hold, by seat
        std::vector<Island>           islands;      ///< the header's first, then by root offset
        std::vector<Abstraction::Gap> holes;
        std::vector<Point>            points;       ///< the open questions, by seat
    };
    /// Run the census alone. Read-only. A clean file is one attached island,
    /// no holes and no open points; one flipped bit opens the point it damaged.
    Census census() const;

private:
    const BYTE* m_base = nullptr;
    Size        m_size = 0;
};

}  // namespace Iris::File

// The generated wire-tag → map-entry vocabulary (spec-derived, emitted by
// generator/ into generated_source/IFE_Map.hpp). It names MapEntryType, so it
// is included after the Abstraction namespace closes; it includes nothing
// itself and assumes IrisFileExtension.hpp was included first.
#include "IFE_Map.hpp"

// =====================================================================
// GLOBAL C-STYLE ALIASES — the file map (advanced) tier
// =====================================================================
// Mirrors the read-tier aliases in IrisFileExtension.hpp. Aliases only.
using IFE_MapEntryType        = Iris::File::Abstraction::MapEntryType;
using IFE_FileMapEntry        = Iris::File::Abstraction::FileMapEntry;
using IFE_FileMap             = Iris::File::Abstraction::FileMap;
using IFE_FieldInfo           = Iris::File::Abstraction::FieldInfo;
using IFE_Gap                 = Iris::File::Abstraction::Gap;
using IFE_GapClass            = Iris::File::Abstraction::GapClass;
using IFE_ProducerFailure     = Iris::File::Abstraction::ProducerFailure;
using IFE_ProducerFailureKind = Iris::File::Abstraction::ProducerFailureKind;

using IFE_Recovery = Iris::File::Recovery;

#endif  // IFE_Recovery_hpp
