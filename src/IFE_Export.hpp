/**
 * @file IFE_Export.hpp
 * @brief Symbol visibility for the Iris File Extension API.
 * @copyright Iris Developers, 2025-2026
 *
 * Extracted from the retired hand-written layer so the generated layer and its
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

// Three states, not two. Building the shared library exports; consuming that
// shared library imports; everything else — a static archive, an object
// library, a translation unit compiled straight into an executable — does
// neither and needs no attribute at all.
//
// Collapsing the last two is what this used to do, and on MSVC it meant that
// "not exporting" produced __declspec(dllimport). Every target that compiled
// IFE_Runtime.cpp without IFE_EXPORT_API then failed with C2491, a definition
// of a dllimport function, and every target that merely *used* an Iris type
// emitted an __imp_ reference to a DLL the build never produced.
#ifndef IFE_EXPORT_API
#define IFE_EXPORT_API      false
#endif
// Set this only when linking against a separately built IFE shared library.
#ifndef IFE_IMPORT_API
#define IFE_IMPORT_API      false
#endif

#ifndef IFE_EXPORT
    #if IFE_EXPORT_API
        #if defined(_MSC_VER)
        #define IFE_EXPORT  __declspec(dllexport)
        #else
        #define IFE_EXPORT  __attribute__ ((visibility ("default")))
        #endif
    #elif IFE_IMPORT_API
        #if defined(_MSC_VER)
        #define IFE_EXPORT  __declspec(dllimport)
        #else
        #define IFE_EXPORT
        #endif
    #else
        // Static or object linkage: no attribute. Default visibility is
        // hidden by CXX_VISIBILITY_PRESET (see CMakeLists).
        #define IFE_EXPORT
    #endif
#endif

#endif  // IFE_Export_hpp
