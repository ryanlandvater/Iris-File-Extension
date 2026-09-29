#ifndef IFE_corpus_path_hpp
#define IFE_corpus_path_hpp

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------
// Whole-file byte loading — the ONE reader every fixture-loading test shares.
//
// This logic was copy-pasted into five test files (load_fixture x3 and
// read_whole_file x4, in two variants). It lives here now; a test keeps at most
// a three-line wrapper, and only where it must charge a failure counter of its
// own.
// ---------------------------------------------------------------------------

/// Read a file whole into a mutable byte buffer. On any failure — missing
/// file, short read — prints why to stderr and returns an EMPTY vector; a
/// fixture is never legitimately empty, so empty is the caller's failure
/// signal. Never throws. (`Iris::BYTE` is `std::uint8_t`, so the return type is
/// the callers' `std::vector<BYTE>`.)
[[nodiscard]] inline std::vector<std::uint8_t> ife_read_file(const std::string& __path) {
    std::FILE* in = std::fopen(__path.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "FAIL: cannot open %s\n", __path.c_str());
        return {};
    }
    std::fseek(in, 0, SEEK_END);
    const long __n = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::vector<std::uint8_t> __bytes(__n > 0 ? static_cast<std::size_t>(__n) : 0u);
    const std::size_t __read = std::fread(__bytes.data(), 1, __bytes.size(), in);
    std::fclose(in);
    if (__read != __bytes.size()) {
        std::fprintf(stderr, "FAIL: short read from %s\n", __path.c_str());
        return {};
    }
    return __bytes;
}

/// Read the corpus fixture @p __name from directory @p __dir. Same failure
/// contract as ife_read_file.
[[nodiscard]] inline std::vector<std::uint8_t> ife_load_fixture(const std::string& __dir,
                                                                const char* __name) {
    return ife_read_file(__dir + "/" + __name);
}

#endif  // IFE_corpus_path_hpp
