# A consumer of the *installed* package, not of this source tree.
#
# The release archives are the deliverable, and nothing else in this repository
# ever resolves IrisFileExtensionConfig.cmake, links the shipped library, or
# includes its headers from include/Iris — the tests and examples link the
# object library in-tree. So this is the only check that what users download is
# more than a correctly named tarball, and on MSVC it is the only place the
# shipped DLL's import library is ever linked at all.
#
# Built by .github/workflows/release.yml against the staging prefix, before the
# archive is made. Run it by hand the same way:
#
#   cmake -B build -S tools/package_smoke -DCMAKE_PREFIX_PATH=<install prefix>
#   cmake --build build --config Release && ./build/ife_package_smoke
cmake_minimum_required(VERSION 3.11...3.30)
project(ife_package_smoke CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CONFIG mode explicitly: a find-module fallback searching the system would
# find some other Iris and report success for a package this release never
# produced.
find_package(IrisFileExtension REQUIRED CONFIG)

add_executable(ife_package_smoke consume.cpp)

# The shared library on purpose. It is the artifact whose symbol visibility can
# be wrong while the static archive is fine, and the one a distributed binary
# package exists to provide.
target_link_libraries(ife_package_smoke PRIVATE IrisFileExtension)
