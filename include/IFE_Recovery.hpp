/**
 * @file IFE_Recovery.hpp
 * @brief Iris File Extension recovery — two-witness reconciliation and gap analysis.
 * @copyright Iris Developers, 2025-2026
 *
 * The recovery subsystem is the RC-1…RC-4 port of FastFHIR's REC-10…18: every
 * parent→child block reference in the file is encoded twice — the parent's
 * slot {expected type, stored offset} and the child's block header
 * {VALIDATION == own offset, RECOVERY tag} — so a single-site corruption
 * leaves the other half as evidence. This header declares the machinery that
 * turns that redundancy into restored references:
 *
 *   Recovery::reachable_blocks() — the offset-chain walk only (clean-stream
 *       baseline; the same traversal generate_file_map performs).
 *   Recovery::scan()             — the byte-wise signature walk (recovery
 *       producer; what recover_file_structure has always been), now sizing
 *       every entry it finds and tiling the arena for gaps. The validated
 *       FILE_HEADER is noted by construction — it has no VALIDATION to find
 *       by signature, and an unnoted root would tile as a Hole on every
 *       clean file.
 *   Recovery::recover()          — both joined: every reference reconciled by
 *       whichever witness survives, every repair classified and costed —
 *       in-place rewrites and repoints ranked on ONE metric, holes matched by
 *       their residual self-signature at a tight band with progressive
 *       expansion, and each repair followed into the subtree it restores
 *       behind the weak (no-wild-pointers) gate (FastFHIR REC-19.6…REC-20.6,
 *       ported RC-7).
 *   Recovery::apply()            — the only path that mutates.
 *
 * The value types (BlockRef, RepairClass, Gap, RecoveryReport, FileMap) live
 * in the ADVANCED tier — "IFE_Advanced.hpp" — which this header includes. The
 * public read surface ("IrisFileExtension.hpp") no longer carries them.
 *
 * THREAT MODEL — bit-flip only (MIGRATION.md RC-4.3). The Hamming ranker
 * assumes a corrupted value stays within a small number of bit flips of the
 * truth (IFE_RECOVERY_MAX_FLIPS). Truncation, memmove, or overwrite damage
 * defeats it; those cases fall back to type + reachability ranking and are
 * reported Ambiguous or Unrecovered, never guessed. Integrity, not
 * authenticity: a repaired file is not the same object as an intact one —
 * every repair is reported, and apply() is the only path that mutates.
 *
 * The "no second witness" boundary: inline scalar attribute values packed in
 * ATTRIBUTE_BYTES; leaf payload bytes (ICC_PROFILE, CLINICAL_METADATA,
 * IMAGE_BYTES, ANNOTATION_BYTES content); TILE_PIXEL_DATA streams (their
 * extent lives only in the tile-offsets entry; the optional frame carries the
 * tile index, not a length); the FILE_HEADER itself (magic, no VALIDATION —
 * the root); and both witnesses damaged on the same reference (a Gap of class
 * Hole: located and sized, content gone). The redundancy is not a guarantee
 * of total recoverability.
 */

#ifndef IFE_Recovery_hpp
#define IFE_Recovery_hpp

#include <cstdint>
#include <vector>

// The ADVANCED tier: the file-map / verdict value types plus the two advanced
// entry points. It includes IrisFileExtension.hpp, so the read surface comes
// with it.
#include "IFE_Advanced.hpp"

namespace Iris::File {

/// The bit-flip budget of the Hamming ranker (RC-1.3, FastFHIR
/// FF_RECOVERY_MAX_FLIPS). A candidate whose repair costs more than this is
/// not a hypothesis, it is a guess.
inline constexpr std::uint32_t IFE_RECOVERY_MAX_FLIPS = 8;

/// The depth bound of the walk and the reapply (RC-6, FastFHIR
/// FF_RECOVERY_MAX_DEPTH). The visited set already bounds total work (each
/// block is enumerated at most once), so this is a second belt: a corrupted
/// chain that never revisits a block cannot run away either.
inline constexpr std::size_t IFE_RECOVERY_MAX_DEPTH = 64;

/**
 * @brief Two-sided reconciliation of block references over arbitrary bytes.
 *
 * Constructing this never dereferences a header field (the scan may be asked
 * to read a file whose header is the damaged part). Every read below is
 * bounds-checked against the buffer size.
 *
 * TWO ENTRY POINTS, ONE PURPOSE EACH:
 *   reachable_blocks() — offset-chain walk only, enumerated as BlockRefs.
 *       The CLEAN-STREAM baseline path. Cost O(blocks), no census.
 *   recover()          — DAMAGED streams only: the byte census (scan) and the
 *       reachability walk in parallel, joined, then the orphan test and
 *       two-sided reconciliation. A baseline must never call this — the
 *       census is pure waste on bytes that are known good.
 */
class IFE_EXPORT Recovery {
public:
    /// Recovery requires the mapped file. There is no (ptr, size) entry point
    /// — it is a scanner over bytes you suspect are damaged, not a reader.
    /// Never dereferences a header field at construction.
    explicit Recovery(const FileAccessInfo& info) noexcept;

    /// Offset-chain walk: every reference an intact graph encodes, from the
    /// root. NULL slots are absence, not damage, and are not enumerated.
    /// Empty when the root is unreadable — recover() still runs on the scan.
    /// Descent is one-surviving-witness (RC-6 / FastFHIR REC-19): a child
    /// whose self-offset is merely within the flip budget is still walked,
    /// under the slot's declared type; only a child with neither witness
    /// stops the walk. On a clean stream every child has both, so the
    /// baseline is unchanged.
    std::vector<Abstraction::BlockRef> reachable_blocks() const;

    /// Byte-wise signature walk (what recover_file_structure has always done):
    /// every self-consistent block — VALIDATION == own offset followed by a
    /// 0x55xx recovery tag, or a 40-bit self-offset tile frame — sized by its
    /// own header (RC-2.1). The signature pass is chunked across
    /// hardware-concurrency threads when the arena is large enough to
    /// amortize the spawn (RC-6 / FastFHIR REC-19.5); the audit, tile
    /// charging and gap sweep are sequential. A self-consistent offset whose
    /// tag is not a known type is recorded on `map.failures`, never silently
    /// dropped. Tile streams are charged to the entries that address them
    /// when those entries survive. The validated FILE_HEADER is noted by
    /// construction (it has no VALIDATION to find; an unnoted root would
    /// tile as a Hole on every clean file). Fills `map.gaps` via find_gaps().
    Abstraction::FileMap scan() const;

    /// Tile the arena: every run of bytes no entry claims is a gap, classed
    /// Hole (damage) / VersionSkew (benign) / Trailing. Exposed so a caller
    /// can re-run it over a repaired map. The map must be offset-ordered (a
    /// FileMap from scan() or generate_file_map() is).
    void find_gaps(Abstraction::FileMap& map) const;

    /// The RC-1 + RC-6 + RC-7 reconciliation: scan + reachability joined,
    /// per-reference verdicts classified with Hamming bit costs, gaps
    /// reported, and every damaged reference the engine saw recorded on
    /// `report.failures` — a subtree lost is visible in the audit even when
    /// no verdict fails (that invisibility was the REC-19 defect this ports).
    /// Never mutates; use apply() for that.
    ///
    /// RC-7 additions, ported from the FastFHIR REC-19/REC-20 rewrite and its
    /// debugging session (recovery_handoff.md):
    ///   * the walk enumerates from parent attestation and descends while
    ///     either witness survives (one-surviving-witness, defects 1/2/5);
    ///   * census blocks the walk never reached are enumerated under their
    ///     own wire type — an orphaned parent's outgoing references are real;
    ///   * CLASSIFICATION IS ONE RANKED DECISION (REC-19.6/REC-20.4): the
    ///     in-place hypothesis (rewrite the child's tag or VALIDATION word)
    ///     competes with the repoint hypothesis (rewrite the parent slot to
    ///     the cheapest unique orphan OR ranked hole position) on ONE metric,
    ///     and equal costs are Ambiguous — never guessed. Shape-based
    ///     decisions were the defect: a 1-bit offset flip onto an innocent
    ///     valid block of another type reads exactly like a flipped tag, and
    ///     only the ranker separates them;
    ///   * TAG CONSENSUS (REC-22.2, ported RC-8): when the child validates but
    ///     carries a tag of ANOTHER plausible type, cost cannot separate
    ///     "the tag was flipped" from "the slot landed on an innocent block"
    ///     (both are one bit) — COHERENCE can. The classifier asks whether
    ///     the block READS as each candidate type (block_reads_as: enumerate
    ///     under the hypothesis, every child must corroborate, silence is no
    ///     opinion). Exactly one coherent reading is decisive: the child's
    ///     header is the damaged copy (adjudicated TagRepaired — apply() may
    ///     rewrite a plausible tag because evidence decided it) or the block
    ///     is innocent (in-place disabled; only a repoint may fix the edge);
    ///   * after classification, every Corroborated / HoleCorroborated /
    ///     TagRepaired / PositionRepaired repair is re-enumerated under the
    ///     CORRECTED type behind the WEAK gate — no wild pointers, damage
    ///     expected (REC-19.7, item 7 of the recovery handoff). The strong
    ///     every-child-corroborates gate is for a block whose TYPE is a
    ///     hypothesis; here the type came from a classified repair, and
    ///     demanding corroboration from a batch whose damaged child is the
    ///     thing being hunted stopped the generational cascade dead at the
    ///     first repair. A Corroborated repair is followed to the ranker's
    ///     candidate, not to the rejected slot value (repaired_target);
    ///   * holes are matched by their residual SELF-SIGNATURE (REC-20.2,
    ///     item 8): every position inside a hole whose damaged VALIDATION
    ///     word is within 2 bits of its own address is a candidate, ranked
    ///     cheapest-first and built once per call. Size-equality was the
    ///     weaker form — it assumed the block started at the hole's first
    ///     byte and could not see a hole holding more than one lost block;
    ///   * PROGRESSIVE BAND EXPANSION (REC-20.5/.6, item 15): references
    ///     still broken after the reapply are re-matched at band 3…8, each
    ///     match followed immediately through the weak gate (item 16: a
    ///     repaired block is a parent nobody has ever looked inside), capped
    ///     at IFE_RECOVERY_MAX_FLIPS so one number governs the engine;
    ///   * holes in the report mean "still missing AFTER recovery": the
    ///     census is re-tiled last, so a hole a repair filled is not
    ///     reported as damage.
    ///
    /// A broken tile-offsets entry is corroborated by the frame-noted stream
    /// it left unnamed (RC-3.4), ranked by hamming distance AND by size: the
    /// entry's claimed extent must end at a claim boundary, or the candidate
    /// is not the child. Frame witnesses beyond the flip budget report
    /// Unrecovered, never Intact. When an edge has lost BOTH witnesses and no
    /// surviving orphan is within budget, a ranked hole position may restore
    /// the pointer (HoleCorroborated, RC-2.4 / REC-20.4) — a restored
    /// pointer to destroyed bytes, never conflated with Corroborated.
    Abstraction::RecoveryReport recover() const;

    /// The only mutating path (RC-1.4 / FastFHIR REC-15): writes back the
    /// classed repairs — Corroborated/HoleCorroborated parent slots,
    /// PositionRepaired and TagRepaired child headers, ExtentDerived array
    /// counts, rebuilt tile-offsets entries. Slot writes are width-aware: a
    /// tile entry's OFFSET is written at its native 40-bit width so the
    /// adjacent u24 SIZE field survives; every other slot is u64.
    ///
    /// ALL-OR-NOTHING, VERIFIED (RC-7): every repair is planned and
    /// range-checked before the first byte is touched, so one repair that
    /// cannot be honoured writes nothing at all. Each committed run is then
    /// re-read and verified, and a write that does not verify rolls the
    /// whole report back to its before-images — the in-place analogue of
    /// FastFHIR's write-into-a-copy, because the damaged original must stay
    /// readable for a before/after comparison. Ambiguous and Unrecovered are
    /// never written — they were reported because the engine declined to
    /// choose, and writing a guess would turn a declared uncertainty into a
    /// silent one. TagRepaired writes carry their own gate (item 18): a tag
    /// that is still a plausible type could be an innocent block's real type
    /// — relabelling it erases the only surviving record of what it was, so
    /// such a verdict is reported and declined, never written — UNLESS the
    /// rewrite was adjudicated (RC-8 / REC-22.2, BlockVerdict::
    /// tag_adjudicated): coherence evidence then answered the innocent-block
    /// worry, and the damaged child header may be repaired even though the
    /// wire tag is plausible.
    bool apply(const Abstraction::RecoveryReport& report) noexcept;

    /// Hamming distance, the ranker's cost function: number of differing bits.
    [[nodiscard]] static std::uint32_t hamming_cost(std::uint64_t a, std::uint64_t b) noexcept;

private:
    const BYTE* m_base = nullptr;
    Size        m_size = 0;
};

}  // namespace Iris::File
#endif  // IFE_Recovery_hpp
