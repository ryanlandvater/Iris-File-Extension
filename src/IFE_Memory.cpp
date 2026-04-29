/**
 * @file IFE_Memory.cpp
 * @brief Implementation of the lock-free VMA substrate. See IFE_Memory.hpp.
 */
#include "IFE_Memory.hpp"

#include <cstring>
#include <new>
#include <system_error>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <winioctl.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #include <errno.h>
#endif

namespace IFE {

namespace detail {

namespace {

// Sanity check: mmap / MapViewOfFile return at minimum page-aligned
// pointers (4 KiB on every platform we support; 16 KiB on some ARM64
// configurations). `std::atomic_ref<uint64_t>::required_alignment` is
// `alignof(uint64_t)` on every conforming implementation but the standard
// permits it to exceed that on exotic platforms; assert that it never
// exceeds a 4 KiB page so the OS-provided alignment is always sufficient.
static_assert(std::atomic_ref<std::uint64_t>::required_alignment <= 4096,
              "atomic_ref<uint64_t>::required_alignment exceeds the smallest "
              "supported OS page size; sparse-mmap arena alignment would not "
              "satisfy the atomic_ref alignment contract on this platform.");

#ifdef _WIN32

[[noreturn]] void throw_winerror(const char* what) {
    const DWORD code = ::GetLastError();
    throw std::system_error(static_cast<int>(code),
                            std::system_category(), what);
}

void* map_anonymous(std::size_t capacity, void*& section_handle_out) {
    // ULARGE split keeps the 64-bit capacity portable across 32/64-bit builds.
    const ULARGE_INTEGER sz{ .QuadPart = static_cast<ULONGLONG>(capacity) };
    HANDLE section = ::CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        sz.HighPart, sz.LowPart, nullptr);
    if (section == nullptr) {
        throw_winerror("IFE::Memory: CreateFileMappingA(anonymous) failed");
    }
    void* view = ::MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, capacity);
    if (view == nullptr) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(section);
        throw std::system_error(static_cast<int>(code),
                                std::system_category(),
                                "IFE::Memory: MapViewOfFile(anonymous) failed");
    }
    section_handle_out = section;
    return view;
}

void* map_file(const std::filesystem::path& path,
               std::size_t                  capacity,
               void*&                       file_handle_out,
               void*&                       section_handle_out) {
    // CreateFileA expects a narrow path. Use std::filesystem's u8string for
    // a portable round-trip; on Windows std::filesystem::path::string()
    // already returns ANSI for native chars.
    const std::string narrow = path.string();
    HANDLE file = ::CreateFileA(
        narrow.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw_winerror("IFE::Memory: CreateFileA failed");
    }
    // Mark sparse so the unwritten tail of the reservation does not consume
    // disk blocks. Failure is non-fatal on filesystems that don't support
    // sparse files (the file just becomes fully allocated).
    DWORD bytes_returned = 0;
    ::DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                      &bytes_returned, nullptr);
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(capacity);
    if (!::SetFilePointerEx(file, end, nullptr, FILE_BEGIN) ||
        !::SetEndOfFile(file)) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(file);
        throw std::system_error(static_cast<int>(code),
                                std::system_category(),
                                "IFE::Memory: SetEndOfFile failed");
    }
    const ULARGE_INTEGER sz{ .QuadPart = static_cast<ULONGLONG>(capacity) };
    HANDLE section = ::CreateFileMappingA(
        file, nullptr, PAGE_READWRITE,
        sz.HighPart, sz.LowPart, nullptr);
    if (section == nullptr) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(file);
        throw std::system_error(static_cast<int>(code),
                                std::system_category(),
                                "IFE::Memory: CreateFileMappingA(file) failed");
    }
    void* view = ::MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, capacity);
    if (view == nullptr) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(section);
        ::CloseHandle(file);
        throw std::system_error(static_cast<int>(code),
                                std::system_category(),
                                "IFE::Memory: MapViewOfFile(file) failed");
    }
    file_handle_out    = file;
    section_handle_out = section;
    return view;
}

#else // POSIX

[[noreturn]] void throw_errno(const char* what) {
    const int code = errno;
    throw std::system_error(code, std::generic_category(), what);
}

void* map_anonymous(std::size_t capacity) {
    void* p = ::mmap(nullptr, capacity,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    if (p == MAP_FAILED) {
        throw_errno("IFE::Memory: mmap(MAP_ANONYMOUS) failed");
    }
    return p;
}

void* map_file(const std::filesystem::path& path,
               std::size_t                  capacity,
               int&                         fd_out) {
    const int fd = ::open(path.c_str(),
                          O_RDWR | O_CREAT | O_CLOEXEC,
                          static_cast<mode_t>(0644));
    if (fd < 0) {
        throw_errno("IFE::Memory: open() failed");
    }
    if (::ftruncate(fd, static_cast<off_t>(capacity)) != 0) {
        const int code = errno;
        ::close(fd);
        throw std::system_error(code, std::generic_category(),
                                "IFE::Memory: ftruncate() failed");
    }
    void* p = ::mmap(nullptr, capacity,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        const int code = errno;
        ::close(fd);
        throw std::system_error(code, std::generic_category(),
                                "IFE::Memory: mmap(MAP_SHARED) failed");
    }
    fd_out = fd;
    return p;
}

#endif // _WIN32

} // namespace

Body::Body(std::size_t capacity) : m_capacity(capacity) {
    if (capacity < WRITE_HEAD_BYTES) {
        throw std::invalid_argument("IFE::Memory: capacity must be >= WRITE_HEAD_BYTES");
    }
#ifdef _WIN32
    m_file_handle = INVALID_HANDLE_VALUE;
    m_arena = static_cast<std::uint8_t*>(map_anonymous(capacity, m_os_handle));
#else
    m_arena = static_cast<std::uint8_t*>(map_anonymous(capacity));
#endif
    seed_cursor();
}

Body::Body(std::size_t capacity, const std::filesystem::path& path)
    : m_capacity(capacity) {
    if (capacity < WRITE_HEAD_BYTES) {
        throw std::invalid_argument("IFE::Memory: capacity must be >= WRITE_HEAD_BYTES");
    }
#ifdef _WIN32
    m_arena = static_cast<std::uint8_t*>(
        map_file(path, capacity, m_file_handle, m_os_handle));
#else
    m_arena = static_cast<std::uint8_t*>(map_file(path, capacity, m_os_fd));
#endif
    seed_cursor();
}

void Body::seed_cursor() {
    // Zero the reserved write-head bytes; the cursor is plain storage so no
    // placement-new is required. (For file-backed mappings this writes the
    // first page through to disk on flush.)
    std::memset(m_arena, 0, WRITE_HEAD_BYTES);
    // Seed the cursor at WRITE_HEAD_BYTES via atomic_ref. No concurrency
    // exists yet (we're inside the constructor) but using the same path
    // here documents the access pattern.
    std::atomic_ref<std::uint64_t>(*cursor_storage())
        .store(WRITE_HEAD_BYTES, std::memory_order_relaxed);
}

Body::~Body() {
    if (m_arena) {
#ifdef _WIN32
        ::UnmapViewOfFile(m_arena);
#else
        ::munmap(m_arena, m_capacity);
#endif
        m_arena = nullptr;
    }
#ifdef _WIN32
    if (m_os_handle) {
        ::CloseHandle(static_cast<HANDLE>(m_os_handle));
        m_os_handle = nullptr;
    }
    if (m_file_handle && m_file_handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(static_cast<HANDLE>(m_file_handle));
    }
    m_file_handle = INVALID_HANDLE_VALUE;
#else
    if (m_os_fd >= 0) {
        ::close(m_os_fd);
        m_os_fd = -1;
    }
#endif
    // Note: SHM segments are intentionally not unlinked here, mirroring
    // FastFHIR's behavior (a restarting service can re-attach). SHM
    // construction itself is a deferred follow-up; this PR ships only
    // anonymous and file-backed modes.
}

bool Body::file_backed() const noexcept {
#ifdef _WIN32
    return m_file_handle != nullptr && m_file_handle != INVALID_HANDLE_VALUE;
#else
    return m_os_fd >= 0;
#endif
}

void Body::truncate_file(std::size_t size) {
    if (!file_backed()) return;
#ifdef _WIN32
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(size);
    HANDLE file = static_cast<HANDLE>(m_file_handle);
    if (!::SetFilePointerEx(file, end, nullptr, FILE_BEGIN) ||
        !::SetEndOfFile(file)) {
        const DWORD code = ::GetLastError();
        throw std::system_error(static_cast<int>(code),
                                std::system_category(),
                                "IFE::Memory::truncate_file: SetEndOfFile failed");
    }
#else
    if (::ftruncate(m_os_fd, static_cast<off_t>(size)) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "IFE::Memory::truncate_file: ftruncate failed");
    }
#endif
}

} // namespace detail

// ---------------------------------------------------------------------------
// StreamHead
// ---------------------------------------------------------------------------
StreamHead::StreamHead(std::uint64_t* cursor_storage,
                       std::uint8_t*  data,
                       std::uint64_t  acquired_offset) noexcept
    : m_cursor(cursor_storage), m_data(data),
      m_acquired_offset(acquired_offset) {}

StreamHead::StreamHead(StreamHead&& other) noexcept
    : m_cursor(other.m_cursor),
      m_data(other.m_data),
      m_acquired_offset(other.m_acquired_offset) {
    other.m_cursor = nullptr;
    other.m_data   = nullptr;
}

StreamHead& StreamHead::operator=(StreamHead&& other) noexcept {
    if (this != &other) {
        release();
        m_cursor          = other.m_cursor;
        m_data            = other.m_data;
        m_acquired_offset = other.m_acquired_offset;
        other.m_cursor    = nullptr;
        other.m_data      = nullptr;
    }
    return *this;
}

StreamHead::~StreamHead() { release(); }

void StreamHead::release() noexcept {
    if (m_cursor) {
        // Atomically clear the stream-lock bit without disturbing the
        // offset; concurrent claim_space calls observe the unlock.
        std::atomic_ref<std::uint64_t>(*m_cursor)
            .fetch_and(STREAM_OFFSET_MASK, std::memory_order_release);
        m_cursor = nullptr;
        m_data   = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
Memory Memory::create(std::size_t capacity) {
    return Memory{std::make_shared<detail::Body>(capacity)};
}

Memory Memory::createFromFile(const std::filesystem::path& path,
                              std::size_t                  capacity) {
    return Memory{std::make_shared<detail::Body>(capacity, path)};
}

void Memory::truncate_file(std::size_t size) {
    if (!m_body) {
        throw std::logic_error("IFE::Memory::truncate_file on empty handle");
    }
    m_body->truncate_file(size);
}

std::uint64_t Memory::write_head() const noexcept {
    if (!m_body) return 0;
    // const_cast: cursor_storage's bytes are mutable (atomic state lives
    // there); std::atomic_ref requires a non-const T&.
    auto* storage = const_cast<std::uint64_t*>(m_body->cursor_storage());
    return std::atomic_ref<std::uint64_t>(*storage)
               .load(std::memory_order_acquire) & STREAM_OFFSET_MASK;
}

bool Memory::stream_locked() const noexcept {
    if (!m_body) return false;
    auto* storage = const_cast<std::uint64_t*>(m_body->cursor_storage());
    return (std::atomic_ref<std::uint64_t>(*storage)
                .load(std::memory_order_acquire) & STREAM_LOCK_BIT) != 0;
}

Reservation Memory::claim_space(std::size_t bytes) {
    if (!m_body) {
        throw std::logic_error("IFE::Memory::claim_space on empty handle");
    }
    if (bytes == 0) {
        throw std::logic_error("IFE::Memory::claim_space requires bytes > 0");
    }

    auto* storage = m_body->cursor_storage();
    std::atomic_ref<std::uint64_t> cursor(*storage);

    // Cooperative wait while a streamer holds the lock bit. We don't try to
    // beat the streamer; we simply yield until the bit clears, then perform
    // the fetch_add. If a streamer acquires *between* the wait and the
    // fetch_add, the offset still advances correctly: the streamer's
    // `acquired_offset` was captured pre-CAS and our reservation is past
    // that point.
    for (;;) {
        std::uint64_t snapshot = cursor.load(std::memory_order_acquire);
        if ((snapshot & STREAM_LOCK_BIT) == 0) break;
        std::this_thread::yield();
    }

    // fetch_add advances the cursor in a single atomic step. The lock bit
    // only occupies bit 63; a `bytes` value that wouldn't fit in 63 bits
    // is already larger than any plausible arena, but guard anyway.
    if (bytes > STREAM_OFFSET_MASK) {
        throw std::bad_alloc{};
    }

    const std::uint64_t prev = cursor.fetch_add(bytes, std::memory_order_acq_rel);
    const std::uint64_t base_offset = prev & STREAM_OFFSET_MASK;
    const std::uint64_t end_offset  = base_offset + bytes;

    if (end_offset > m_body->capacity()) {
        // Roll the cursor back. If a concurrent claim already advanced past
        // us, we cannot safely revert a hole; report exhaustion regardless,
        // and the application is expected to abandon the build.
        cursor.fetch_sub(bytes, std::memory_order_acq_rel);
        throw std::bad_alloc{};
    }

    Reservation s;
    s.offset = base_offset;
    s.size   = bytes;
    s.ptr    = m_body->data() + base_offset;
    return s;
}

StreamHead Memory::acquire_stream() {
    if (!m_body) {
        throw std::logic_error("IFE::Memory::acquire_stream on empty handle");
    }
    auto* storage = m_body->cursor_storage();
    std::atomic_ref<std::uint64_t> cursor(*storage);

    for (;;) {
        std::uint64_t expected = cursor.load(std::memory_order_acquire);
        if ((expected & STREAM_LOCK_BIT) != 0) {
            // Another streamer holds the lock; spin.
            std::this_thread::yield();
            continue;
        }
        const std::uint64_t desired = expected | STREAM_LOCK_BIT;
        if (cursor.compare_exchange_weak(expected, desired,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            return StreamHead{storage,
                              m_body->data() + (expected & STREAM_OFFSET_MASK),
                              expected & STREAM_OFFSET_MASK};
        }
        // CAS failed: someone advanced the offset (or grabbed the lock).
        // Loop and retry.
    }
}

} // namespace IFE
