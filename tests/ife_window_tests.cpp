/**
 * @file ife_window_tests.cpp
 * @brief The residency abstraction, including the path no developer machine runs.
 *
 * The remote mode exists for the browser, and a browser is exactly where a
 * test does not run. That is why Window takes its transport as a function
 * pointer: the fetch is a seam, and everything on this side of it — bounds,
 * cache hits, partial overlaps, transport failure — is driven here with a stub
 * that serves bytes from an array. What remains untested by this file is the
 * ranged HTTP request itself, which is one function in one `#if` block.
 *
 * Self-contained; non-zero exit on failure.
 */
#include "IFE_Window.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

using ::IFE::BYTE;
using ::IFE::Offset;
using ::IFE::Size;

constexpr Size FILE_SIZE = 1024;

/// Stands in for the network. Serves byte i as (i % 251) — prime, so a
/// mis-addressed read produces a value that could not belong at that offset.
struct Stub {
    std::vector<BYTE> bytes;
    int  calls   = 0;
    Size supplied = 0;
    bool fail    = false;

    Stub() : bytes(FILE_SIZE) {
        for (Size i = 0; i < FILE_SIZE; ++i) bytes[i] = static_cast<BYTE>(i % 251);
    }

    static bool fetch(void* user, Offset offset, Size size, BYTE* dst) {
        Stub& self = *static_cast<Stub*>(user);
        ++self.calls;
        if (self.fail) return false;
        self.supplied += size;
        std::memcpy(dst, self.bytes.data() + offset, size);
        return true;
    }
};

BYTE expected_at(Offset i) { return static_cast<BYTE>(i % 251); }

void test_resident_is_pointer_arithmetic() {
    Stub source;
    auto window = IFE::Window::resident(source.bytes.data(), FILE_SIZE);

    IFE_CHECK(window.is_resident());
    IFE_CHECK(window.size() == FILE_SIZE);
    IFE_CHECK(window.map(0, FILE_SIZE) == source.bytes.data());
    IFE_CHECK(window.map(100, 8) == source.bytes.data() + 100);
    IFE_CHECK(window.cached_bytes() == 0);      // resident mode caches nothing
    IFE_CHECK(source.calls == 0);               // and never fetches
}

void test_bounds_are_refused_in_both_modes() {
    Stub source;
    auto resident = IFE::Window::resident(source.bytes.data(), FILE_SIZE);
    auto remote   = IFE::Window::remote(FILE_SIZE, &Stub::fetch, &source);

    for (IFE::Window* w : {&resident, &remote}) {
        IFE_CHECK(w->map(FILE_SIZE, 1) == nullptr);          // starts at EOF
        IFE_CHECK(w->map(FILE_SIZE - 4, 5) == nullptr);      // ends past EOF
        IFE_CHECK(w->map(FILE_SIZE + 1, 0) == nullptr);      // offset past EOF
        // The check must not wrap: offset + size overflowing Offset would
        // otherwise land back inside the file and be granted.
        IFE_CHECK(w->map(8, ~Size{0}) == nullptr);
        IFE_CHECK(w->map(~Offset{0}, 8) == nullptr);

        // The exact end of the file is legal, and so is an empty range.
        IFE_CHECK(w->map(FILE_SIZE - 4, 4) != nullptr);
        IFE_CHECK(w->map(FILE_SIZE, 0) != nullptr);
    }
    IFE_CHECK(source.fail == false);
}

void test_remote_fetches_once_and_serves_from_cache() {
    Stub source;
    auto window = IFE::Window::remote(FILE_SIZE, &Stub::fetch, &source);
    IFE_CHECK(!window.is_resident());

    const BYTE* first = window.map(64, 32);
    IFE_CHECK(first != nullptr);
    IFE_CHECK(source.calls == 1);
    IFE_CHECK(window.cached_bytes() == 32);
    for (Size i = 0; i < 32; ++i) IFE_CHECK(first[i] == expected_at(64 + i));

    // The same range again, and a strict subrange, both come from the page.
    IFE_CHECK(window.map(64, 32) == first);
    IFE_CHECK(window.map(70, 4) == first + 6);
    IFE_CHECK(source.calls == 1);

    // A range the page only partly covers is a miss: a partially satisfied
    // read is exactly what map must never return.
    const BYTE* wider = window.map(64, 64);
    IFE_CHECK(wider != nullptr);
    IFE_CHECK(source.calls == 2);
    for (Size i = 0; i < 64; ++i) IFE_CHECK(wider[i] == expected_at(64 + i));
}

void test_cached_pointers_survive_cache_growth() {
    Stub source;
    auto window = IFE::Window::remote(FILE_SIZE, &Stub::fetch, &source);

    // Hold the first page, then force many more. The pages vector reallocates;
    // the bytes must not move with it, or every handle built on an earlier
    // page is silently dangling. This is the reason a Page owns its own
    // allocation instead of holding a std::vector inline.
    const BYTE* held = window.map(0, 16);
    IFE_CHECK(held != nullptr);
    for (Offset at = 16; at + 16 <= FILE_SIZE; at += 16)
        IFE_CHECK(window.map(at, 16) != nullptr);

    IFE_CHECK(source.calls == static_cast<int>(FILE_SIZE / 16));
    for (Size i = 0; i < 16; ++i) IFE_CHECK(held[i] == expected_at(i));
}

void test_transport_failure_is_null_not_garbage() {
    Stub source;
    source.fail = true;
    auto window = IFE::Window::remote(FILE_SIZE, &Stub::fetch, &source);

    IFE_CHECK(window.map(0, 16) == nullptr);
    IFE_CHECK(source.calls == 1);
    // A failed fetch must not be cached, or one network blip would poison the
    // range for the life of the Window.
    IFE_CHECK(window.cached_bytes() == 0);

    source.fail = false;
    const BYTE* recovered = window.map(0, 16);
    IFE_CHECK(recovered != nullptr);
    IFE_CHECK(source.calls == 2);
    for (Size i = 0; i < 16; ++i) IFE_CHECK(recovered[i] == expected_at(i));
}

void test_remote_without_transport_fails_closed() {
    // Null transport is a programming error, not a reason to hand back a
    // pointer into nothing.
    auto window = IFE::Window::remote(FILE_SIZE, nullptr, nullptr);
    IFE_CHECK(window.map(0, 16) == nullptr);
}

}  // namespace

int main() {
    test_resident_is_pointer_arithmetic();
    test_bounds_are_refused_in_both_modes();
    test_remote_fetches_once_and_serves_from_cache();
    test_cached_pointers_survive_cache_growth();
    test_transport_failure_is_null_not_garbage();
    test_remote_without_transport_fails_closed();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_window_tests: all checks passed\n");
    return 0;
}
