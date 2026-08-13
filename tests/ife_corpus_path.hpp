#ifndef IFE_corpus_path_hpp
#define IFE_corpus_path_hpp

#include <filesystem>
#include <string>

/// Resolve a test's corpus argument to a DIRECTORY, whichever form it came
/// in. CMake/CTest passes the corpus directory itself; Bazel runfiles cannot
/// hand a test a directory, so BUILD.bazel passes the runfiles path of one
/// corpus FILE and the directory is its parent (both corpus files sit in the
/// same runfiles directory). The directory case is unchanged behaviour for
/// CTest; the file case is what makes the Bazel targets work.
inline std::string ife_corpus_dir(const char* __arg) {
    const std::filesystem::path __p(__arg);
    return std::filesystem::is_directory(__p) ? __p.string()
                                              : __p.parent_path().string();
}

#endif  // IFE_corpus_path_hpp
