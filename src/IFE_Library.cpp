/**
 * @file IFE_Library.cpp
 * @brief The wrapper targets' one compiled unit.
 *
 * IrisFileExtension (shared) and IrisFileExtensionStatic exist only to
 * package IrisFileExtensionLib's objects; they compile nothing of their own.
 * CMake's Xcode generator emits no Sources phase for such a target, so no
 * link is ever scheduled and the wrapper "builds" to nothing — the
 * exported-symbols test found the missing dylib. This translation unit gives
 * each wrapper a real compiled source, which is what makes the Xcode
 * generator run the link. Internal linkage keeps the anchor out of the ABI
 * on every platform.
 */
namespace {
/// Compiled into the wrapper targets so every generator schedules their link.
[[maybe_unused]] const char IFE_LibraryAnchor[] = "IFE";
}  // namespace
