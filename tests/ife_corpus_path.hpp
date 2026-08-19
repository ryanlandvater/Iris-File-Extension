#ifndef IFE_corpus_path_hpp
#define IFE_corpus_path_hpp

#include <filesystem>
#include <string>

// Bazel runs Windows tests in manifest-only mode (RUNFILES_MANIFEST_ONLY=1,
// see bazel's launcher.cc): there is NO runfiles tree, so cwd-relative paths
// fail there while working on POSIX. Resolve through the runfiles library
// instead — it reads the manifest on Windows and the tree on POSIX.
#ifdef IFE_BAZEL_RUNFILES
#include <memory>
#include "rules_cc/cc/runfiles/runfiles.h"
#endif

/// Resolve a test's corpus argument to a DIRECTORY, whichever form it came
/// in. CMake/CTest passes the corpus directory itself; Bazel passes the
/// manifest-style runfiles path of one corpus FILE (BUILD.bazel:
/// "_main/tests/corpus/v1_0_witness.test_slide"), which is resolved through
/// the runfiles library — required on Windows. The directory is the resolved
/// file's parent (both corpus files sit in the same runfiles directory). The
/// directory case is unchanged behaviour for CTest.
inline std::string ife_corpus_dir(const char* __arg) {
    std::string __p = __arg;
#ifdef IFE_BAZEL_RUNFILES
    // CreateForTest reads RUNFILES_MANIFEST_FILE / TEST_SRCDIR. Rlocation
    // returns empty for paths it does not know (e.g. CMake's absolute
    // directory argument, never reached in Bazel builds), in which case the
    // argument is used as-is.
    std::string error;
    const std::unique_ptr<rules_cc::cc::runfiles::Runfiles> rf(
        rules_cc::cc::runfiles::Runfiles::CreateForTest(&error));
    if (rf) {
        const std::string rloc = rf->Rlocation(__p);
        if (!rloc.empty()) __p = rloc;
    }
#endif
    const std::filesystem::path __path(__p);
    return std::filesystem::is_directory(__path) ? __path.string()
                                                 : __path.parent_path().string();
}

#endif  // IFE_corpus_path_hpp
