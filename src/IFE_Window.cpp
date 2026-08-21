/**
 * @file IFE_Window.cpp
 * @brief The one translation unit in the project that knows about the browser.
 * @copyright Iris Developers, 2025-2026
 *
 * Everything platform-specific about *where bytes live* is the single
 * `#if defined(__EMSCRIPTEN__)` at the bottom of this file. The caching and
 * bounds logic above it is platform-independent and is exercised natively by
 * tests/ife_window_tests.cpp through a stub transport.
 */

#include "IFE_Window.hpp"

#include <cstring>

namespace IFE {

Window Window::resident(const BYTE* __base, Size __size) noexcept {
    Window window;
    window.__base = __base;
    window.__size = __size;
    return window;
}

Window Window::remote(Size __size, Fetch __fetch, void* __user) noexcept {
    Window window;
    window.__size  = __size;
    window.__fetch = __fetch;
    window.__user  = __user;
    return window;
}

Size Window::cached_bytes() const noexcept {
    Size total = 0;
    for (const Page& page : __pages) total += page.size;
    return total;
}

const BYTE* Window::map(Offset __offset, Size __request) {
    // Bounds first, and in a form that cannot wrap: a caller asking for a
    // range that overflows Offset must not be turned into a small in-range
    // request by the addition.
    if (__offset > __size || __request > __size - __offset) return nullptr;

    if (__base != nullptr) return __base + __offset;   // resident
    if (__fetch == nullptr) return nullptr;            // remote with no transport

    // A page already covering the whole request. Linear: a slide read touches
    // a handful of blocks, and an index would cost more to maintain than the
    // scan saves.
    for (const Page& page : __pages)
        if (__offset >= page.begin && __offset + __request <= page.begin + page.size)
            return page.bytes.get() + (__offset - page.begin);

    // Fetch exactly what was asked for. Deliberately no read-ahead: the
    // runtime knows its access pattern and this layer does not, so guessing
    // here would be policy in the wrong place.
    Page page;
    page.begin = __offset;
    page.size  = __request;
    page.bytes = std::make_unique<BYTE[]>(__request ? __request : 1);

    if (!__fetch(__user, __offset, __request, page.bytes.get())) return nullptr;

    __pages.push_back(std::move(page));
    return __pages.back().bytes.get();
}

}  // namespace IFE

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
// MARK: - The only platform branch in the project's residency path
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
#if defined(__EMSCRIPTEN__)

#include <emscripten/emscripten.h>

#include <cstdlib>
#include <string>

// Bridges to JavaScript's fetch, appearing synchronous to C++ via Asyncify:
// issue a ranged request, write the status and length back through pointers,
// return a malloc'd payload the caller frees.
EM_ASYNC_JS(int, ife_fetch_range_async,
            (const char* url_ptr, const char* range_ptr, int* size_ptr, int* status_ptr), {
    const url_js   = UTF8ToString(url_ptr);
    const range_js = UTF8ToString(range_ptr);
    try {
        const response = await fetch(new Request(url_js, { headers: { 'Range': range_js } }));
        HEAP32[status_ptr >> 2] = response.status;
        if (!response.ok) { HEAP32[size_ptr >> 2] = 0; return 0; }
        const buffer = await response.arrayBuffer();
        const bytes  = new Uint8Array(buffer);
        const ptr    = _malloc(bytes.length);
        HEAPU8.set(bytes, ptr);
        HEAP32[size_ptr >> 2] = bytes.length;
        return ptr;
    } catch (error) {
        console.error("IFE: ranged fetch failed:", error);
        HEAP32[size_ptr >> 2]   = 0;
        HEAP32[status_ptr >> 2] = 0;
        return 0;
    }
});

namespace IFE {

bool fetch_http_range(void* __url, Offset __offset, Size __size, BYTE* __dst) {
    if (__url == nullptr || __size == 0) return __size == 0;

    // Inclusive end, as HTTP requires.
    const std::string range = "bytes=" + std::to_string(__offset) + "-" +
                              std::to_string(__offset + __size - 1);

    int   length = 0;
    int   status = 0;
    char* payload = reinterpret_cast<char*>(ife_fetch_range_async(
        static_cast<const char*>(__url), range.c_str(), &length, &status));

    // 206 Partial Content is the only success: a 200 means the server ignored
    // the Range header and sent the whole file, which would overrun __dst.
    const bool ok = payload != nullptr && status == 206 &&
                    static_cast<Size>(length) == __size;
    if (ok) std::memcpy(__dst, payload, __size);
    if (payload) std::free(payload);
    return ok;
}

}  // namespace IFE

#endif  // __EMSCRIPTEN__
