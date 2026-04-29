/**
 * @file IFE_Memory.hpp
 * @brief Lock-free Virtual Memory Arena (VMA) substrate for the Iris File
 *        Extension. Phase 1 of the FastFHIR substrate migration.
 *
 * This header is **dormant** unless built with `IFE_USE_FASTFHIR_SUBSTRATE`.
 * It introduces no on-disk format change and no change to the existing
 * `IrisCodec::Abstraction::File` API. Phases 2-6 progressively wire the
 * substrate into the read/write paths.
 *
 * Design (ported from the FastFHIR `FF_Memory` Handle/Body pattern):
 *   - Handle/Body split: `Memory` is a thin handle around a
 *     `std::shared_ptr<Body>`. The `Body` owns the byte arena and the
 *     64-bit atomic write-head.
 *   - The first 16 bytes of the arena are reserved for the atomic write-head.
 *     Bytes [0,8) hold a plain `uint64_t` accessed exclusively through
 *     `std::atomic_ref<uint64_t>` (the project rule is to never alias raw
 *     storage as `std::atomic<T>*`). Bytes [8,16) reserved for future
 *     Phase 4 `FILE_HEADER` migration. The cursor's low 63 bits hold
 *     the next write offset; bit 63 is the `STREAM_LOCK_BIT`.
 *   - `claim_space(bytes)` performs a `fetch_add` so concurrent ingestion
 *     threads never contend on a mutex. While the stream-lock bit is set,
 *     writers cooperatively spin until the streamer releases it.
 *   - `StreamHead` is an RAII guard that CAS-acquires the lock bit for
 *     exclusive raw-DMA tile streaming; concurrent `claim_space` calls block
 *     until the guard is destroyed.
 *   - `Memory::View` is a `std::shared_ptr<const Body>` that pins the arena
 *     for the lifetime of an asynchronous read so network/disk I/O cannot
 *     observe a freed buffer.
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#ifndef IFE_Memory_hpp
#define IFE_Memory_hpp

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace IFE {

/// Total bytes reserved at the start of the arena for the atomic write-head.
/// Bytes [0,8): atomic cursor (low 63 bits = offset, bit 63 = stream lock).
/// Bytes [8,16): reserved for the Phase 4 `FILE_HEADER` migration.
constexpr std::size_t WRITE_HEAD_BYTES = 16;

/// High bit of the 64-bit cursor; when set, exclusive stream mode is active
/// and concurrent `claim_space` calls must wait.
constexpr std::uint64_t STREAM_LOCK_BIT = 1ULL << 63;

/// Mask for the offset portion of the cursor (low 63 bits).
constexpr std::uint64_t STREAM_OFFSET_MASK = STREAM_LOCK_BIT - 1ULL;

/// A reserved [offset, offset+size) byte run returned by `claim_space`.
/// (Phase 5 introduced the `IFE::Span<T>` read-side template; this writable
/// reservation handle is therefore named `Reservation` rather than `Span`.)
struct Reservation {
    std::uint64_t offset = 0;  ///< Absolute arena offset of the slice's first byte.
    std::size_t   size   = 0;  ///< Length of the slice in bytes.
    std::uint8_t* ptr    = nullptr; ///< Direct pointer into the arena.
    constexpr bool valid() const noexcept { return ptr != nullptr; }
};

class Memory; // forward

namespace detail {

/// Body owns the arena allocation and the atomic cursor. Held via shared_ptr
/// so async I/O can pin it through `Memory::View`.
class Body {
public:
    Body(std::size_t capacity);
    ~Body();

    Body(const Body&)            = delete;
    Body& operator=(const Body&) = delete;
    Body(Body&&)                 = delete;
    Body& operator=(Body&&)      = delete;

    std::uint8_t*       data() noexcept       { return m_arena; }
    const std::uint8_t* data() const noexcept { return m_arena; }
    std::size_t         capacity() const noexcept { return m_capacity; }

    /// Pointer to the plain `uint64_t` storage for the cursor at offset 0
    /// of the arena. All atomic access goes through a `std::atomic_ref<u64>`
    /// constructed on demand by callers — matching the FastFHIR reference
    /// implementation (`FF_Memory_t::m_head_ptr`), which exposes only a
    /// plain `uint64_t*` and lets each callsite wrap it as
    /// `std::atomic_ref<uint64_t> head(*ptr)` at point of use. The project
    /// rule is to never expose `std::atomic<T>*` over arena bytes.
    std::uint64_t* cursor_storage() noexcept {
        return reinterpret_cast<std::uint64_t*>(m_arena);
    }
    const std::uint64_t* cursor_storage() const noexcept {
        return reinterpret_cast<const std::uint64_t*>(m_arena);
    }

private:
    std::uint8_t* m_arena    = nullptr;
    std::size_t   m_capacity = 0;
};

} // namespace detail

/**
 * @brief RAII guard that acquires the `STREAM_LOCK_BIT` for exclusive raw
 *        DMA-style streaming over the arena. While alive, concurrent
 *        `claim_space` calls cooperatively spin.
 *
 * Move-only. The guard is non-copyable so the lock cannot be double-released.
 */
class StreamHead {
public:
    StreamHead() = default;
    StreamHead(const StreamHead&)            = delete;
    StreamHead& operator=(const StreamHead&) = delete;
    StreamHead(StreamHead&& other) noexcept;
    StreamHead& operator=(StreamHead&& other) noexcept;
    ~StreamHead();

    /// True if this guard currently holds the stream lock.
    bool held() const noexcept { return m_cursor != nullptr; }

    /// Current write-head offset captured at acquisition time. Stable for
    /// the lifetime of the guard because `claim_space` cannot advance it.
    std::uint64_t offset() const noexcept { return m_acquired_offset; }

    /// Direct pointer to the streamable region (arena base + offset).
    std::uint8_t* data() const noexcept { return m_data; }

    /// Release the lock early. Idempotent.
    void release() noexcept;

private:
    friend class Memory;
    StreamHead(std::uint64_t* cursor_storage,
               std::uint8_t*  data,
               std::uint64_t  acquired_offset) noexcept;

    /// Plain `uint64_t*` to the cursor storage in the arena. All atomic
    /// access wraps it in a `std::atomic_ref<u64>` at point of use.
    std::uint64_t* m_cursor          = nullptr;
    std::uint8_t*  m_data            = nullptr;
    std::uint64_t  m_acquired_offset = 0;
};

/**
 * @brief Thin Handle around a `shared_ptr<Body>` providing the lock-free VMA.
 *
 * The handle is cheap to copy; copies share the same underlying arena and
 * cursor. A null-constructed `Memory{}` is empty and all methods on it are
 * no-ops or throw.
 */
class Memory {
public:
    /// Construct an empty handle. Use `Memory::create(capacity)` for a real arena.
    Memory() = default;

    /// Allocate a new arena of `capacity` bytes. `capacity` must be at least
    /// `WRITE_HEAD_BYTES`. The first 16 bytes are zeroed (cursor = 0).
    static Memory create(std::size_t capacity);

    /// True if the handle owns a body.
    explicit operator bool() const noexcept { return static_cast<bool>(m_body); }

    /// Total capacity of the arena.
    std::size_t capacity() const noexcept { return m_body ? m_body->capacity() : 0; }

    /// Direct pointer to the arena base (use carefully).
    std::uint8_t*       data() noexcept       { return m_body ? m_body->data() : nullptr; }
    const std::uint8_t* data() const noexcept { return m_body ? m_body->data() : nullptr; }

    /// Current write-head offset (low 63 bits of the cursor).
    std::uint64_t write_head() const noexcept;

    /// True if the stream-lock bit is currently set.
    bool stream_locked() const noexcept;

    /**
     * @brief Reserve `bytes` of arena space using a single atomic `fetch_add`
     *        on the low 63 bits. Cooperatively spins while the stream-lock
     *        bit is set.
     *
     * @throws std::bad_alloc if the reservation would exceed the arena capacity
     *         (the cursor is rolled back so subsequent calls remain consistent).
     * @throws std::logic_error if `bytes == 0` or this handle is empty.
     */
    Reservation claim_space(std::size_t bytes);

    /**
     * @brief Acquire exclusive stream mode. Spins on a CAS that sets bit 63
     *        without disturbing the offset bits.
     */
    StreamHead acquire_stream();

    /**
     * @brief A non-owning lifetime extension for the body. Holds the
     *        `shared_ptr` so async I/O completing after the originating
     *        `Memory` handle has been dropped still observes a valid arena.
     */
    class View {
    public:
        View() = default;
        explicit View(std::shared_ptr<const detail::Body> body) noexcept
            : m_body(std::move(body)) {}

        explicit operator bool() const noexcept { return static_cast<bool>(m_body); }
        const std::uint8_t* data() const noexcept { return m_body ? m_body->data() : nullptr; }
        std::size_t capacity() const noexcept { return m_body ? m_body->capacity() : 0; }

    private:
        std::shared_ptr<const detail::Body> m_body;
    };

    /// Snapshot the body for asynchronous I/O. The returned `View` keeps
    /// the arena alive even if all `Memory` handles are destroyed.
    View view() const noexcept { return View{m_body}; }

private:
    explicit Memory(std::shared_ptr<detail::Body> body) noexcept
        : m_body(std::move(body)) {}

    std::shared_ptr<detail::Body> m_body;
};

} // namespace IFE

#endif // IFE_Memory_hpp
