/**
 * @file IFE_Window.hpp
 * @brief Residency: where a block's bytes come from, resolved in one place.
 * @copyright Iris Developers, 2025-2026
 *
 * The generated block handles hold `const BYTE*` and nothing else. They never
 * learn whether those bytes came from an mmap or a network range request, and
 * they must never contain a platform branch — `__EMSCRIPTEN__` appears zero
 * times anywhere in generated_source, and stays that way.
 *
 * This is the difference from v1, and it is worth stating precisely because
 * the v1 shape is the thing being deliberately not reproduced. There,
 * residency was injected into fifteen separate readers: each block carried a
 * `check_and_fetch_remote` that fetched its own header, re-pointed its own
 * `__offset` into the fetched buffer, re-fetched at full size, and then
 * rewrote the *caller's* `__base` through a `const_cast`
 * (`src/IrisCodecExtension.cpp` — `LAYER_EXTENTS::check_and_fetch_remote` and
 * `FILE_HEADER`'s near-copy of it). Every reader began by calling it. The
 * hand-written pair carries 116 `__EMSCRIPTEN__` branches; sixteen generated
 * blocks would have carried more.
 *
 * Here, residency is resolved *before* a handle is constructed. The runtime
 * asks the Window for a range, gets a pointer good for exactly that range, and
 * builds a handle on it. One `#if defined(__EMSCRIPTEN__)`, in
 * IFE_Window.cpp, and no `const_cast` anywhere.
 */

#ifndef IFE_Window_hpp
#define IFE_Window_hpp

#include <cstddef>
#include <memory>
#include <vector>

#include "IFE_Bytes.hpp"

namespace IFE {

/// A view of a slide file's bytes, wherever they physically are.
///
/// Two modes, chosen at construction and never re-tested per access:
///   - **resident** — the whole file is already addressable (a native mmap).
///     `map` is a bounds check and a pointer addition.
///   - **remote** — ranges are fetched on demand through a caller-supplied
///     transport and cached for the lifetime of the Window.
///
/// Not copyable: it owns fetched pages, and two Windows over one file would
/// cache the same bytes twice.
class Window {
public:
    /// Read `__size` bytes at `__offset` into `__dst`; false if the range
    /// could not be obtained. A function pointer rather than a virtual: the
    /// implementation is chosen once, it is called once per cache miss rather
    /// than per access, and — the reason that matters here — it is a seam a
    /// native test can drive, so the caching and bounds logic is exercised on
    /// a developer machine instead of only in a browser.
    using Fetch = bool (*)(void* __user, Offset __offset, Size __size, BYTE* __dst);

    /// The whole file is addressable already.
    static Window resident(const BYTE* __base, Size __size) noexcept;

    /// The file is remote; `__fetch` obtains ranges and `__user` is passed
    /// back to it untouched.
    static Window remote(Size __size, Fetch __fetch, void* __user) noexcept;

    Window(Window&&) noexcept            = default;
    Window& operator=(Window&&) noexcept = default;
    Window(const Window&)                = delete;
    Window& operator=(const Window&)     = delete;

    /// A pointer to `__size` contiguous bytes at `__offset`, or nullptr.
    ///
    /// Null means the range is not available — past the end of the file, or a
    /// transport failure — and is never a partially satisfied read. Callers
    /// need no separate error channel: a generated handle built on nullptr
    /// reports `Check::NOT_CONSTRUCTED`, which is already how a truncated file
    /// surfaces.
    ///
    /// A zero-size request is legal and yields a non-null pointer whenever the
    /// offset itself is within the file, so an empty byte array is not an
    /// error.
    [[nodiscard]] const BYTE* map(Offset __offset, Size __size);

    [[nodiscard]] Size size() const noexcept { return __size; }

    /// Whether `map` can ever fail for a reason other than bounds.
    [[nodiscard]] bool is_resident() const noexcept { return __base != nullptr; }

    /// Bytes currently held in the cache. Zero when resident.
    [[nodiscard]] Size cached_bytes() const noexcept;

private:
    Window() noexcept = default;

    /// One fetched range. The bytes live in their own allocation rather than
    /// inside the vector's element, so a pointer handed out by `map` stays
    /// valid when the cache grows.
    struct Page {
        Offset                    begin = 0;
        Size                      size  = 0;
        std::unique_ptr<BYTE[]>   bytes;
    };

    const BYTE*       __base  = nullptr;   ///< non-null in resident mode
    Size              __size  = 0;         ///< total file size
    Fetch             __fetch = nullptr;
    void*             __user  = nullptr;
    std::vector<Page> __pages;
};

/// The Emscripten transport: a ranged HTTP request, as v1 performs it.
/// Declared unconditionally so the header has no platform branch; defined only
/// in the Emscripten build, where `__user` is the URL as a `const char*`.
/// Linking against it elsewhere is a build error rather than a silent stub.
bool fetch_http_range(void* __url, Offset __offset, Size __size, BYTE* __dst);

}  // namespace IFE

#endif  // IFE_Window_hpp
