/**
 * @file IFE_Recovery.hpp
 * @brief Iris File Extension recovery: the census (MIGRATION.md RB-3).
 * @copyright Iris Developers, 2025-2026
 *
 * The advanced tier. The engine is FastFHIR's (`../FastFHIR/src/FF_Recovery.cpp`,
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
#include <vector>

#include "IFE_Advanced.hpp"

namespace Iris::File {

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

using IFE_Recovery = Iris::File::Recovery;

#endif  // IFE_Recovery_hpp
