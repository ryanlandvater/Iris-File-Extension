/**
 * @file IFE_Builder.hpp
 * @brief Iris File Extension — the WRITE body behind `Iris::File::Builder`.
 *
 * TIERS. FastFHIR's shape, applied to IFE's write side:
 *
 *   - PUBLIC  — `Iris::File::Builder` (declared in "IrisFileExtension.hpp"): a
 *               `std::shared_ptr<Builder_t>` handle. Its `.` methods are the
 *               application's write API — declare the tile table, append tiles
 *               and images, finalize. Code does not normally include this header.
 *   - BODY    — `Iris::File::Builder_t` (here): the heap-owned writer. It holds
 *               an `Iris::Memory` VMA and the arena's base pointer, and owns the
 *               claim/store arithmetic. Its block tier — `claim`,
 *               `append(XxxCreateInfo)`, `seal` — is for code that assembles
 *               blocks by hand (fixtures, tests). Reach it with `builder->…` by
 *               including THIS header.
 *
 * WHY THE SPLIT. `Builder_t` is only forward-declared in the public header, so
 * `builder->append(...)` resolves to nothing until "IFE_Builder.hpp" completes
 * the type — the compiler enforces the tier.
 *
 * Mirrors FastFHIR's `Builder` / `Builder_t` and the read side's
 * `Parser` / `Parser_t`.
 *
 * BACKING STORE. `Builder_t` writes into an `Iris::Memory` — a HUGE sparse
 * reservation (BuilderCreateInfo::capacity; only touched pages cost RAM or disk)
 * whose base pointer never moves. Growth is pages materialising on first touch
 * inside that range, so a block, once written, keeps its offset forever and
 * there is no grow, realloc or remap step. The arena is `Iris::Memory` in
 * Iris-Headers (priv/IrisMemory.hpp).
 *
 * CAPACITY. Exhausting the reservation is terminal: `claim` throws. Nothing is
 * ever remapped — the lock-free claim depends on the base never moving — so the
 * answer to "the file may be larger" is a larger reservation, chosen by the
 * caller at `Builder::create`.
 *
 * THE HEADER REGION. The FILE_HEADER lives at offset 0 and is written last, by
 * `seal` (which `finalize` ends with). The builder reserves its bytes at
 * construction, so no claim can ever land where the header will go.
 *
 * @copyright Iris Developers, 2025-2026
 */

#ifndef IFE_Builder_hpp
#define IFE_Builder_hpp

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "IrisFileExtension.hpp"   // the Builder handle, the block vocabulary, FileAccessInfo
#include "IrisMemory.hpp"          // priv: Iris::Memory (the VMA)

namespace Iris::File {

/**
 * @brief The write body behind @ref Builder. Internal tier.
 *
 * Not constructed directly by consumers: `Builder::create` builds one and hands
 * back a handle. Every block goes in through the one `append` template, which
 * drives the generated `size_of` / `store` and returns the block's offset. Wire
 * a parent to its children by passing the child offsets into the parent's
 * `XxxCreateInfo` — the file's offsets are the caller's to place, exactly as
 * they are today, but the claim arithmetic and bounds are the builder's.
 *
 * THREADING. `claim()` is lock-free: the head is a `std::atomic` bumped with a
 * compare-exchange loop, so many producer threads may append concurrently and
 * each gets a distinct, whole range (the model the encoder's tile streams need).
 * `store()`/`size_of()` write only the range they claimed, so appends do not
 * race; `finalize()` is the single-threaded seal and must run once all appends
 * have joined.
 */
class Builder_t
{
    friend class Builder;

    /// One grid position. `offset` is UNWRITTEN until the tile is accounted
    /// for, then either the stream's anchor or NULL_TILE — once, by CAS.
    struct TileSlot {
        std::atomic<std::uint64_t> offset{UNWRITTEN};
        std::uint32_t              size = 0;
    };
    /// Private: not a u40 value, so it can never collide with a wire offset.
    static constexpr std::uint64_t UNWRITTEN = std::numeric_limits<std::uint64_t>::max();

    Iris::Memory m_memory;      ///< the VMA (shared handle; keeps the mapping alive)
    /// The backing file, kept as the caller gave it (empty = anonymous). Held
    /// here rather than recovered from the arena's narrow `name()`, which is
    /// lossy for a non-ASCII path on Windows.
    std::filesystem::path m_path;
    /// Frame tiles of single-plane layers (Z-stacked layers are always framed).
    bool m_tile_frames = true;
    /// The arena base. Never moves while the builder is open; null once a
    /// file-backed builder has been finalized and its mapping released.
    ::Iris::File::BYTE* m_base;
    /// Next free offset (lock-free bump). Starts past the FILE_HEADER, whose
    /// bytes are reserved for `finalize`.
    std::atomic<::Iris::File::Offset> m_head{::Iris::File::blocks::FILE_HEADER::header_size};
    /// Set by `finalize`/`seal`; every later write throws.
    std::atomic<bool> m_sealed{false};

    // ---- the tile table (set once by set_tile_table) ---------------------- //
    std::mutex                     m_table_mutex;  ///< serialises set_tile_table
    std::atomic<bool>              m_table_ready{false};
    BuilderTileTableInfo           m_table;        ///< `planes` normalised to one per layer
    std::vector<std::uint64_t>     m_layer_first;  ///< global index of each layer's tile 0, + total
    std::unique_ptr<TileSlot[]>    m_slots;        ///< one per global tile index

    // ---- associated images ----------------------------------------------- //
    std::mutex                          m_images_mutex;
    std::vector<blocks::ImageEntry>     m_images;
    std::set<std::string>               m_image_labels;

    /// The global tile index of (layer, tile); throws for a position the
    /// table does not have, or before set_tile_table.
    std::uint64_t global_index(std::uint32_t layer, std::uint32_t tile) const;
    void      ensure_open(const char* what) const;

public:
    /// Preconditions: `memory` refers to a live, writable mapping, backed by
    /// `info.filepath` when one is given.
    explicit Builder_t(const Iris::Memory& memory, const BuilderCreateInfo& info = {});

    Builder_t(const Builder_t&)            = delete;
    Builder_t& operator=(const Builder_t&) = delete;

    /// The arena base. Offsets handed back by `append` are relative to it.
    /// Null after a file-backed `finalize` (the mapping is released); an
    /// anonymous builder keeps its bytes readable here after `finalize`.
    [[nodiscard]] ::Iris::File::BYTE* base() const noexcept { return m_base; }
    /// Whether `finalize` / `seal` has run. A sealed builder accepts no writes.
    [[nodiscard]] bool sealed() const noexcept { return m_sealed.load(std::memory_order_acquire); }
    /// The committed write head: the offset the next block will land at.
    [[nodiscard]] ::Iris::File::Offset head() const noexcept {
        return m_head.load(std::memory_order_acquire);
    }
    /// The arena's reserved extent in bytes.
    [[nodiscard]] ::Iris::File::Size capacity() const noexcept { return m_memory.capacity(); }
    /// The arena handle.
    [[nodiscard]] const Iris::Memory& memory() const noexcept { return m_memory; }

    // ---- the application tier (forwarded by the Builder handle) ----------- //

    void   set_tile_table  (const BuilderTileTableInfo& info);
    Offset append_tile     (std::uint32_t layer, std::uint32_t tile, const BYTE* data,
                            Size size, std::uint16_t z_planes);
    void   append_null_tile(std::uint32_t layer, std::uint32_t tile);
    Offset append_image    (const AssociatedImageInfo& info, const BYTE* data, Size size);
    void   finalize        (const BuilderFinalizeInfo& info);

    // ---- the block tier (reached with `builder->…`) ----------------------- //

    /**
     * @brief Reserve @p bytes at the head and return the claimed offset.
     *
     * Lock-free and thread-safe: the head is bumped with a compare-exchange
     * loop, so concurrent callers each get a distinct whole range. A claim that
     * would pass the arena's capacity throws rather than writing out of bounds.
     * That is terminal by design: the reservation is never remapped. A claim on
     * a sealed builder throws `std::logic_error`.
     */
    ::Iris::File::Offset claim(::Iris::File::Size bytes);

    /**
     * @brief Write one block and return its offset.
     *
     * `size_of` gives the block's extent, `claim` reserves it, and `store`
     * writes and self-validates it. The write is checked: a `store` that
     * reports a failure is a bug in the CreateInfo, not a recoverable state,
     * so it throws with the failing block named.
     */
    template <typename T_Info>
    ::Iris::File::Offset append(const T_Info& info)
    {
        const ::Iris::File::Offset offset = claim(::Iris::File::blocks::size_of(info));
        const ::Iris::File::blocks::Status status = ::Iris::File::blocks::store(m_base, offset, info);
        if (!status)
            throw std::runtime_error("IFE Builder: store failed for block at offset " +
                                     std::to_string(offset) +
                                     " (status block '" + std::string(status.block) +
                                     "', field '" + std::string(status.field) + "')");
        return offset;
    }

    /**
     * @brief Write the FILE_HEADER at offset 0 and seal: the block tier's end.
     *
     * `finalize` calls this after writing the structure; a caller assembling
     * blocks by hand calls it directly. @p header must carry every header
     * field except `FILE_SIZE`, which the builder fills with its committed
     * head — the one field the writer knows and the caller should not restate.
     *
     * File-backed: the mapping is released FIRST, then the file is truncated
     * from its reservation to the committed size. The order matters on Windows,
     * where `SetEndOfFile` refuses a file that still has a mapped view; and a
     * closed file is what the caller needs to move it. Without the truncate a
     * written slide fails its own `validate_file_structure`. Anonymous:
     * nothing is on disk; the bytes stay readable through `base()`.
     *
     * Single-threaded: call once every append has joined.
     */
    void seal(::Iris::File::blocks::FileHeaderCreateInfo header);
};

}  // namespace Iris::File

#endif  // IFE_Builder_hpp
