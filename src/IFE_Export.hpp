/**
 * @file IFE_Export.hpp
 * @brief Symbol visibility for the Iris File Extension API.
 * @copyright Iris Developers, 2025-2026
 *
 * Extracted from src/IrisCodecExtension.hpp so the hand-written layer and its
 * generated successor share one definition rather than two that can drift.
 * The semantics are unchanged: CMake sets IFE_EXPORT_API=true when building
 * the library, and consumers get the import side.
 *
 * **What is deliberately not exported.**
 *
 * The exported surface is the *semantic* API — the four entry points and the
 * `IrisCodec::Abstraction` structs — and nothing else. The generated block
 * layer (`IFE::blocks`, `IFE::vtables`, `IFE::constants`) carries no export
 * marking at all, so it is hidden in a shared build, on purpose:
 *
 *   - It is what a consumer actually calls. `examples/slide_info_abstraction.cpp`
 *     builds against the generated stack touching only the entry points and
 *     the abstraction structs; it never names a block handle.
 *   - The block layer is pure field arithmetic. Inlining a `u24` load beats
 *     calling it through a shared-library boundary, and a cross-boundary call
 *     is all an exported accessor could offer.
 *   - It keeps the ABI at a handful of symbols instead of the ~139 the
 *     hand-written `Serialization::` types export today. Every exported
 *     symbol is a thing that cannot change without breaking a consumer, and
 *     the block layer is generated — it *should* be free to change when the
 *     schema does.
 *
 * A consumer that wants the block layer defines `IFE_HEADER_ONLY` before
 * including `IFE_Blocks.hpp`, or compiles `generated_source/IFE_Blocks.cpp`
 * into its own target. That is the supported route, not a workaround.
 */

#ifndef IFE_Export_hpp
#define IFE_Export_hpp

// Should we export the IFE API for low-level calls to the IFE bytestream.
// If being compiled as a part of another project and you do not want to
// or need to export calls that directly manipulate the byte stream,
// set this preprocessor macro to false.
#ifndef IFE_EXPORT_API
#define IFE_EXPORT_API      false
#endif
#if IFE_EXPORT_API
    #ifndef IFE_EXPORT
    #if defined(_MSC_VER)
    #define IFE_EXPORT      __declspec(dllexport)
    #else
    #define IFE_EXPORT      __attribute__ ((visibility ("default")))
#endif
    #endif
#else
    #ifndef IFE_EXPORT
    #if defined(_MSC_VER)
    #define IFE_EXPORT      __declspec(dllimport)
    #else
    #define IFE_EXPORT      // Default is hidden (see CMakeLists)
    #endif
    #endif
#endif

#endif  // IFE_Export_hpp
