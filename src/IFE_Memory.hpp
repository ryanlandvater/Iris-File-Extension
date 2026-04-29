/**
 * @file IFE_Memory.hpp
 * @brief Lock-free Virtual Memory Arena (VMA) substrate for the Iris File
 *        Extension. Phase 1 of the FastFHIR substrate migration.
 *
 * This header defines the substrate memory layer used by the 2.x retooled
 * implementation. Legacy `IrisCodec::Abstraction::File` APIs remain linkable
 * during cutover but are deprecated.
 *
 * Design (ported from the FastFHIR `FF_Memory` Handle/Body pattern):
 *   - Handle/Body split: `Memory` is a thin handle around a
 *     `std::shared_ptr<Body>`. The `Body` owns the byte arena and the
 *     64-bit atomic write-head.
 *   - **Storage substrate is a sparse OS mapping**, not a heap arena. On
 *     POSIX the arena is `mmap(MAP_PRIVATE|MAP_ANONYMOUS)` for in-memory
 *     mode and `open` + `ftruncate` + `mmap(MAP_SHARED)` for file-backed
 *     mode. On Windows it is `CreateFileMappingA(INVALID_HANDLE_VALUE)` +
 *     `MapViewOfFile` for in-memory mode and `CreateFileA` +
 *     `FSCTL_SET_SPARSE` + `CreateFileMappingA(hFile)` + `MapViewOfFile`
 *     for file-backed mode. Capacity is the *reserved* virtual size; the
 *     kernel commits physical pages on first touch, so a 4 GiB reservation
 *     costs ~0 RSS until written. File-backed mappings persist via the
 *     page cache (zero-copy: no `write(2)` after the fact).
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
#include <filesystem>
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
///
/// The arena is backed by a sparse OS mapping (anonymous mmap / Windows
/// page-file backed mapping) by default, or by a sparse file mapping when
/// constructed with a path. Heap allocation is intentionally not used: a
/// large `capacity` is a *reservation* and physical pages are committed
/// only on first touch.
class Body {
public:
    /// Tag type selecting the open-existing constructor below. Disambiguates
    /// the path-taking signatures so callers can't accidentally pick the
    /// fresh-write factory when they meant to resume an existing file.
    struct OpenExistingTag {};

    /// Construct an anonymous, sparse, writable arena of `capacity` bytes.
    explicit Body(std::size_t capacity);

    /// Construct a file-backed, sparse, writable arena of `capacity` bytes
    /// mapped over `path`. The file is created (or extended) to `capacity`
    /// and the mapping is `MAP_SHARED` so writes flow through the page
    /// cache to disk.
    Body(std::size_t capacity, const std::filesystem::path& path);

    /// Open an existing file-backed arena for read+resume. The file must
    /// already exist; capacity is taken from the on-disk file size and the
    /// mapping is `MAP_SHARED` (POSIX) / `MapViewOfFile(...,FILE_MAP_ALL_ACCESS)`
    /// (Windows). The atomic write-head bytes are NOT zeroed — the persisted
    /// cursor is read straight from disk, exactly the semantics needed to
    /// resume a previously finalised arena.
    ///
    /// Throws `std::system_error` on I/O failure or `std::invalid_argument`
    /// if the file is shorter than `WRITE_HEAD_BYTES` (cannot host an arena).
    Body(const std::filesystem::path& path, OpenExistingTag);

    ~Body();

    Body(const Body&)            = delete;
    Body& operator=(const Body&) = delete;
    Body(Body&&)                 = delete;
    Body& operator=(Body&&)      = delete;

    std::uint8_t*       data() noexcept       { return m_arena; }
    const std::uint8_t* data() const noexcept { return m_arena; }
    std::size_t         capacity() const noexcept { return m_capacity; }

    /// True if the arena is mapped over a file (file-backed mode).
    bool file_backed() const noexcept;

    /// Truncate the underlying file to `size` bytes. No-op for anonymous
    /// arenas. Used at finalize to trim the sparse reservation back to the
    /// actually-written extent.
    void truncate_file(std::size_t size);

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
    /// Common initialization: zero the reserved write-head bytes and seed
    /// the cursor at `WRITE_HEAD_BYTES`. Called from both constructors after
    /// the OS mapping has been established.
    void seed_cursor();

    std::uint8_t* m_arena    = nullptr;
    std::size_t   m_capacity = 0;
#ifdef _WIN32
    // Windows: section handle (always set) and optional file handle (only
    // for file-backed mode). `void*` keeps <windows.h> out of this header.
    void* m_os_handle   = nullptr; // HANDLE for the section
    void* m_file_handle = nullptr; // HANDLE for the file (INVALID_HANDLE_VALUE if anonymous)
#else
    // POSIX: file descriptor (-1 for anonymous). The mapping is unmapped
    // via `munmap` regardless; `m_os_fd` is closed only when set.
    int m_os_fd = -1;
#endif
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

    /// Allocate a new anonymous, sparse arena of `capacity` bytes. `capacity`
    /// must be at least `WRITE_HEAD_BYTES`. The first 16 bytes are zeroed
    /// (cursor = WRITE_HEAD_BYTES). Backed by `mmap(MAP_PRIVATE|MAP_ANONYMOUS)`
    /// on POSIX or `CreateFileMappingA(INVALID_HANDLE_VALUE)` + `MapViewOfFile`
    /// on Windows; physical pages are committed lazily on first touch.
    static Memory create(std::size_t capacity);

    /// Allocate a new file-backed, sparse arena of `capacity` bytes mapped
    /// over `path`. The file is created (or truncated to `capacity`) and
    /// mapped `MAP_SHARED` (POSIX) / via `CreateFileMappingA(hFile)` +
    /// `MapViewOfFile` (Windows) so writes persist to disk through the
    /// kernel page cache without a subsequent `write(2)`. The cursor is
    /// re-seeded at `WRITE_HEAD_BYTES`; this factory always treats the
    /// mapping as a fresh write-target. See `openFromFile` for the
    /// read/resume counterpart that resumes the cursor in-place.
    static Memory createFromFile(const std::filesystem::path& path,
                                 std::size_t capacity);

    /**
     * @brief Open an existing IFE file for read + resume.
     *
     * Counterpart to `createFromFile`. The file must exist and:
     *
     *   1. Be at least `WRITE_HEAD_BYTES + DATA_BLOCK_HEADER_SIZE` bytes
     *      long (room for the persisted cursor and a FILE_HEADER preamble).
     *   2. Carry a valid FILE_HEADER block at byte `WRITE_HEAD_BYTES`
     *      (validation slot == `IFE_FILE_MAGIC`, recovery tag ==
     *      `RECOVERY_TAG::RESOURCE_HEADER`).
     *   3. Have a persisted cursor (bytes [0,8)) consistent with the
     *      on-disk file size — i.e. the file has been finalised by
     *      `Builder::finalize()` (or another writer that obeys the same
     *      contract).
     *
     * The mapping is read+writable at the file's existing size; further
     * `claim_space` calls will fail with `std::bad_alloc` because there is
     * no spare reservation past the persisted extent. The intended use is
     * pairing with `IFE::Reflective::Node` for zero-copy reads.
     *
     * @throws std::system_error    on `open` / `mmap` / Windows API failure.
     * @throws std::invalid_argument if the file is too small to host an arena
     *                               or carries an invalid FILE_HEADER preamble.
     * @throws std::runtime_error   if the persisted cursor disagrees with
     *                              the file size (truncated / corrupt file).
     */
    static Memory openFromFile(const std::filesystem::path& path);

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

    /// Truncate the underlying file to `size` bytes. No-op for anonymous
    /// arenas. Use at finalize to trim the sparse reservation back to the
    /// actually-written extent (typically `write_head()`).
    /// @throws std::logic_error if this handle is empty.
    /// @throws std::system_error on `ftruncate`/`SetEndOfFile` failure.
    void truncate_file(std::size_t size);

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
