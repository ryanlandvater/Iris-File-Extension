/**
 * @file IFE_Memory.cpp
 * @brief Implementation of the lock-free VMA substrate. See IFE_Memory.hpp.
 */
#include "IFE_Memory.hpp"

#include <cstring>
#include <new>
#include <thread>

namespace IFE {

namespace detail {

Body::Body(std::size_t capacity) : m_capacity(capacity) {
    if (capacity < WRITE_HEAD_BYTES) {
        throw std::invalid_argument("IFE::Memory: capacity must be >= WRITE_HEAD_BYTES");
    }
    // Allocate aligned to the atomic word so reinterpret_cast<atomic*> is
    // legal and lock-free on common platforms.
    constexpr std::size_t align = alignof(std::atomic<std::uint64_t>);
    void* raw = ::operator new(capacity, std::align_val_t{align});
    m_arena = static_cast<std::uint8_t*>(raw);
    // Zero-initialize the cursor + reserved bytes so the atomic starts at 0.
    std::memset(m_arena, 0, WRITE_HEAD_BYTES);
    // Construct the atomic in place. Using placement new avoids any
    // assumption that std::atomic<uint64_t> is trivially-constructible.
    ::new (static_cast<void*>(m_arena)) std::atomic<std::uint64_t>(WRITE_HEAD_BYTES);
}

Body::~Body() {
    if (m_arena) {
        // Destroy the in-place atomic before releasing the storage.
        reinterpret_cast<std::atomic<std::uint64_t>*>(m_arena)->~atomic();
        constexpr std::size_t align = alignof(std::atomic<std::uint64_t>);
        ::operator delete(static_cast<void*>(m_arena), std::align_val_t{align});
        m_arena = nullptr;
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// StreamHead
// ---------------------------------------------------------------------------
StreamHead::StreamHead(std::atomic<std::uint64_t>* cursor,
                       std::uint8_t* data,
                       std::uint64_t acquired_offset) noexcept
    : m_cursor(cursor), m_data(data), m_acquired_offset(acquired_offset) {}

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
        m_cursor->fetch_and(STREAM_OFFSET_MASK, std::memory_order_release);
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

std::uint64_t Memory::write_head() const noexcept {
    if (!m_body) return 0;
    return m_body->cursor()->load(std::memory_order_acquire) & STREAM_OFFSET_MASK;
}

bool Memory::stream_locked() const noexcept {
    if (!m_body) return false;
    return (m_body->cursor()->load(std::memory_order_acquire) & STREAM_LOCK_BIT) != 0;
}

Reservation Memory::claim_space(std::size_t bytes) {
    if (!m_body) {
        throw std::logic_error("IFE::Memory::claim_space on empty handle");
    }
    if (bytes == 0) {
        throw std::logic_error("IFE::Memory::claim_space requires bytes > 0");
    }

    auto* cursor = m_body->cursor();

    // Cooperative wait while a streamer holds the lock bit. We don't try to
    // beat the streamer; we simply yield until the bit clears, then perform
    // the fetch_add. If a streamer acquires *between* the wait and the
    // fetch_add, the offset still advances correctly: the streamer's
    // `acquired_offset` was captured pre-CAS and our reservation is past
    // that point.
    for (;;) {
        std::uint64_t snapshot = cursor->load(std::memory_order_acquire);
        if ((snapshot & STREAM_LOCK_BIT) == 0) break;
        std::this_thread::yield();
    }

    // fetch_add advances the cursor in a single atomic step. The lock bit
    // only occupies bit 63; a `bytes` value that wouldn't fit in 63 bits
    // is already larger than any plausible arena, but guard anyway.
    if (bytes > STREAM_OFFSET_MASK) {
        throw std::bad_alloc{};
    }

    const std::uint64_t prev = cursor->fetch_add(bytes, std::memory_order_acq_rel);
    const std::uint64_t base_offset = prev & STREAM_OFFSET_MASK;
    const std::uint64_t end_offset  = base_offset + bytes;

    if (end_offset > m_body->capacity()) {
        // Roll the cursor back. If a concurrent claim already advanced past
        // us, we cannot safely revert a hole; report exhaustion regardless,
        // and the application is expected to abandon the build.
        cursor->fetch_sub(bytes, std::memory_order_acq_rel);
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
    auto* cursor = m_body->cursor();

    for (;;) {
        std::uint64_t expected = cursor->load(std::memory_order_acquire);
        if ((expected & STREAM_LOCK_BIT) != 0) {
            // Another streamer holds the lock; spin.
            std::this_thread::yield();
            continue;
        }
        const std::uint64_t desired = expected | STREAM_LOCK_BIT;
        if (cursor->compare_exchange_weak(expected, desired,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            return StreamHead{cursor,
                              m_body->data() + (expected & STREAM_OFFSET_MASK),
                              expected & STREAM_OFFSET_MASK};
        }
        // CAS failed: someone advanced the offset (or grabbed the lock).
        // Loop and retry.
    }
}

} // namespace IFE
