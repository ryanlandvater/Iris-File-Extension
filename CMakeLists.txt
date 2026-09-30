# CMake for Iris File Extension
# 2025 Copyright Ryan Landvater
# Created 3/3/2025

cmake_minimum_required(VERSION 3.11...3.30)
include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(IFE_get_version)
    # The schema document is the single source of truth. The generated
    # constants derive from it (IFE_SCHEMA_VERSION_* / VERSION_WRITTEN), and
    # the retired hand-written header no longer exists to parse.
    set(IFE_spec_identity "${CMAKE_CURRENT_SOURCE_DIR}/spec/ife_header.json")
    if (NOT EXISTS ${IFE_spec_identity})
        message(FATAL_ERROR "Couldn't find spec/ife_header.json!")
    endif()

    file(READ ${IFE_spec_identity} identity)

    if (identity MATCHES "\"major\"[ ]*:[ ]*([0-9]+)")
        set(MAJOR_VERSION "${CMAKE_MATCH_1}")
    else ()
        message(FATAL_ERROR "Couldn't get the IFE major version from spec/ife_header.json")
    endif()

    if (identity MATCHES "\"minor\"[ ]*:[ ]*([0-9]+)")
        set(MINOR_VERSION "${CMAKE_MATCH_1}")
    else ()
        message(FATAL_ERROR "Couldn't get the IFE minor version from spec/ife_header.json")
    endif()

    set(VERSION_STRING "${MAJOR_VERSION}.${MINOR_VERSION}" PARENT_SCOPE)
endfunction()
IFE_get_version()
message(STATUS "BUILDING ACCORDING TO IRIS FILE EXTENSION (IFE) SPECIFICATION v" ${VERSION_STRING})

Project(
    IrisFileExtension 
    LANGUAGES CXX 
    VERSION ${VERSION_STRING}
)

option(IRIS_EXTENSION_BUILD_ALL "Convenience option to build all targets" OFF)

option(IFE_BUILD_SHARED "Build shared Iris File Extension Libraries" ON)
option(IFE_BUILD_STATIC "Build static Iris File Extension Libraries" ON)
option(IFE_BUILD_EXAMPLES "Build shared Iris File Extension Example implementations" OFF)
option(IFE_BUILD_TESTS "Build IFE unit/stress tests" OFF)
option(IFE_RUN_GENERATOR "Regenerate generated_source/ at configure (requires generator/)" OFF)
# Disables ONLY the corpus conformance test target. It never disables the
# corpus fetch: the oracle, the runtime tests and the >4 GiB test all read
# the fetched snapshot, so a build without it is not a build.
option(IFE_CORPUS "Build the corpus conformance test (ife_corpus_tests)" ON)
if(IRIS_EXTENSION_BUILD_ALL)
    set (IFE_BUILD_SHARED ON)
    set (IFE_BUILD_STATIC ON)
    set (IFE_BUILD_EXAMPLES ON)
endif(IRIS_EXTENSION_BUILD_ALL)

set(IFE_SOURCE_DIR ${PROJECT_SOURCE_DIR}/src)
# Public headers live in include/, implementation (compiled .cpp) in src/.
# The install surface is the include/ FILE_SET; src/ is never installed.
set(IFE_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if (NOT DEFINED irisheaders_SOURCE_DIR)
    # Fetch dependencies into a fixed, gitignored directory in the source tree
    # rather than inside whichever build directory happens to be in use.  The
    # headers are then always at .deps/irisheaders-src/include, so a syntax
    # check, an editor, or a one-off compiler invocation can find them without
    # knowing where the build lives -- while .gitignore guarantees they can
    # never be committed.
    #
    # Only when IFE is the top-level project: a parent consuming this repo owns
    # its own dependency layout and must not have it redirected underneath it.
    if (PROJECT_IS_TOP_LEVEL)
        set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
    endif()
    # ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    # Iris-Headers source selection — fork-mirrored.
    # ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    # IFE and Iris-Headers move together: a change here can depend on an
    # unreleased change there.  Landing that change in the Org's Iris-Headers
    # would alter the dependency every downstream consumer (Iris-Codec, ...)
    # already builds against.  So each fork fetches its OWN Iris-Headers — a
    # checkout of ryanlandvater/Iris-File-Extension fetches
    # ryanlandvater/Iris-Headers, a checkout of IrisDigitalPathology/... fetches
    # the Org's — and an unreleased pair is proved in an insulated fork before
    # either side reaches the Org.  The owner is taken from this repo's `origin`
    # remote, so the two can never cross.  A tarball with no origin, or any
    # unusual layout, overrides with -DIFE_IRIS_HEADERS_OWNER=<owner> or a full
    # -DIFE_IRIS_HEADERS_REPOSITORY=<url>.
    set(IFE_IRIS_HEADERS_OWNER "" CACHE STRING
        "GitHub owner to fetch Iris-Headers from (empty: derive from origin)")
    set(IFE_IRIS_HEADERS_GIT_TAG "main" CACHE STRING
        "Iris-Headers ref to fetch")
    set(IFE_IRIS_HEADERS_REPOSITORY "" CACHE STRING
        "Full Iris-Headers git URL (empty: derive from the owner)")

    if (NOT IFE_IRIS_HEADERS_REPOSITORY)
        if (NOT IFE_IRIS_HEADERS_OWNER)
            # GitHub Actions exports GITHUB_REPOSITORY=owner/repo, which is
            # authoritative in CI (the runner's origin remote is unreliable).
            if (DEFINED ENV{GITHUB_REPOSITORY})
                string(REGEX REPLACE "/.*$" "" IFE_IRIS_HEADERS_OWNER
                       "$ENV{GITHUB_REPOSITORY}")
            else()
                execute_process(
                    COMMAND git remote get-url origin
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE _ife_origin
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _ife_origin_rc)
                if (_ife_origin_rc EQUAL 0 AND
                    _ife_origin MATCHES "github\\.com[:/]([^/]+)/")
                    set(IFE_IRIS_HEADERS_OWNER "${CMAKE_MATCH_1}")
                else()
                    set(IFE_IRIS_HEADERS_OWNER "IrisDigitalPathology")
                endif()
            endif()
        endif()
        set(IFE_IRIS_HEADERS_REPOSITORY
            "https://github.com/${IFE_IRIS_HEADERS_OWNER}/Iris-Headers.git")
    endif()

    message(STATUS "IFE: Iris-Headers <- ${IFE_IRIS_HEADERS_REPOSITORY} "
                   "@ ${IFE_IRIS_HEADERS_GIT_TAG}")
    FetchContent_Declare (
        IrisHeaders
        GIT_REPOSITORY "${IFE_IRIS_HEADERS_REPOSITORY}"
        GIT_TAG "${IFE_IRIS_HEADERS_GIT_TAG}"
        GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(IrisHeaders)
endif()

# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# Warnings (RC-10.5) — mirrored in BUILD.bazel's _COPTS
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# -Wall and -Wextra are the new coverage. -Wswitch is named for a different
# reason: it is already on by default in clang and gcc, so naming it is a
# GUARD, not an addition -- it survives someone trimming -Wall, and it states
# that a new enum member a switch must handle fails at compile time rather
# than being silently uncounted (FastFHIR's gap tally had exactly that bug).
#
# Directory scope, deliberately placed AFTER FetchContent_MakeAvailable above:
# add_compile_options only reaches targets created from here on, so the
# Iris-Headers subproject keeps its own flags. The two Iris-Headers sources
# compiled straight into IrisFileExtensionLib do inherit these -- they are
# clean today, and nothing here is -Werror, so upstream can never break this
# build with a warning.
#
# NOT -Werror. Every first-party translation unit is warning-free as of
# RC-10.5; making that a hard error would turn a compiler upgrade into a
# build outage, which is how projects end up deleting the flags entirely.
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wswitch)
endif()

# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# Iris File Extension Universal Object Build
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
set (
    IFE_SourcesExport 
    ${IFE_INCLUDE_DIR}/IrisFileExtension.hpp
    ${IFE_INCLUDE_DIR}/IFE_Bytes.hpp
    ${IFE_INCLUDE_DIR}/IFE_Primitives.hpp
    ${IFE_INCLUDE_DIR}/IFE_Recovery.hpp
    ${IFE_INCLUDE_DIR}/IFE_Builder.hpp
    ${IFE_INCLUDE_DIR}/IFE_Parser.hpp
    # The two Iris-Headers headers the public surface includes. IrisCore.hpp
    # and IrisCodecCore.hpp are runtime/codec concerns IFE does not use —
    # they stay with Iris-Headers' own package, not this one.
    ${irisheaders_SOURCE_DIR}/include/IrisTypes.hpp
    ${irisheaders_SOURCE_DIR}/include/IrisCodecTypes.hpp
)
# IFE_Window.{hpp,cpp} is Emscripten-only (the WASM remote windowed fetch)
# and the installed package is native, so it is not exported; source-tree
# consumers (FetchContent, WASM) still get it via IFE_SourcesPriv.
set (
    IFE_SourcesPriv
    ${IFE_SOURCE_DIR}/IFE_Runtime.cpp
    ${IFE_SOURCE_DIR}/IFE_Recovery.cpp
    ${IFE_SOURCE_DIR}/IFE_Builder.cpp
    ${IFE_SOURCE_DIR}/IFE_Parser.cpp
    ${IFE_SOURCE_DIR}/IFE_Window.cpp
    ${irisheaders_SOURCE_DIR}/src/IrisBuffer.cpp
    # Iris::MemoryArena's OS calls.  Split out of priv/IrisMemory.hpp so that
    # windows.h -- and the PLANES/TRANSPARENT/ERROR/IN/OUT macros the lint
    # table exists for -- stays out of every consumer's translation unit.
    ${irisheaders_SOURCE_DIR}/src/IrisMemory.cpp
)
set (
    IFE_IncludeDir
    ${irisheaders_SOURCE_DIR}/include
    ${irisheaders_SOURCE_DIR}/priv
    ${IFE_INCLUDE_DIR}
)
set (
    IFE_Dependencies
    IrisHeaders
)
# The Builder is appended to from many std::threads (ife_builder_layered_tests
# does exactly that), and the recovery census being rebuilt scans on them.
# Threads::Threads is the portable way to get -pthread on glibc and the no-op
# on macOS/Windows.
find_package(Threads REQUIRED)
list(APPEND IFE_Dependencies Threads::Threads)
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# Generated source (generated_source/) and generated docs
# (generated_docs/) — generator pipeline output. Both are gitignored and
# freely regenerable; the committed spec JSON is the source of truth.
# Generation runs only when the pipeline exists, and only then if
# IFE_RUN_GENERATOR=ON or the anchor file is missing.
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
set(IFE_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_source")
set(IFE_DOCS_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/generated_docs")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/generator/pipeline.py)
    if(IFE_RUN_GENERATOR OR NOT EXISTS "${IFE_GENERATED_DIR}/IFE_Blocks.hpp")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -m generator
                    --out-dir ${IFE_GENERATED_DIR}
                    --docs-dir ${IFE_DOCS_DIR}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE IFE_GEN_RESULT
        )
        if(NOT IFE_GEN_RESULT EQUAL 0)
            message(FATAL_ERROR "IFE code generator failed — see output above")
        endif()
    endif()
endif()
# Single GLOB per configure, always after the generator block so a fresh
# checkout with an empty generated_source/ still compiles what the
# generator just produced.
file(GLOB IFE_GENERATED_SOURCES CONFIGURE_DEPENDS ${IFE_GENERATED_DIR}/*.cpp)
# The conformance layer is a development tool, not a production
# dependency: it ships as its own target so an application links
# it only when it wants validation.  Keep it out of the library's sources.
set(IFE_VALIDATION_SOURCE ${IFE_GENERATED_DIR}/IFE_Validation.cpp)
list(REMOVE_ITEM IFE_GENERATED_SOURCES ${IFE_VALIDATION_SOURCE})
file(GLOB IFE_GENERATED_HEADERS CONFIGURE_DEPENDS ${IFE_GENERATED_DIR}/*.hpp)
# The conformance layer is a development tool: its header ships only with
# the validation library (which this repo does not install), never with the
# base install.
list(FILTER IFE_GENERATED_HEADERS EXCLUDE REGEX "IFE_Validation\\.hpp$")
if(IFE_GENERATED_SOURCES OR IFE_GENERATED_HEADERS)
    list(APPEND IFE_SourcesPriv   ${IFE_GENERATED_SOURCES})
    list(APPEND IFE_SourcesExport ${IFE_GENERATED_HEADERS})
    list(APPEND IFE_IncludeDir    ${IFE_GENERATED_DIR})
    message(STATUS "IFE: compiling generated sources from ${IFE_GENERATED_DIR}")
endif()
# Generated headers live under their own base dir for the FILE_SET; keep
# the base list conditional so an empty generated_source/ (before the first
# generation) never references a nonexistent directory.
set(IFE_FILE_SET_BASE_DIRS ${IFE_INCLUDE_DIR})
# The two Iris-Headers headers above are under their own base dir.
list(APPEND IFE_FILE_SET_BASE_DIRS ${irisheaders_SOURCE_DIR}/include)
if(IFE_GENERATED_HEADERS)
    list(APPEND IFE_FILE_SET_BASE_DIRS ${IFE_GENERATED_DIR})
endif()
add_library(IrisFileExtensionLib OBJECT)
# Deliberately no link to the IrisHeaders target: the exported package would
# then carry Iris-Headers' whole install (IrisCore.hpp, IrisCodecCore.hpp —
# downstream runtime/codec concerns). The two headers IFE's public surface
# includes are on the include paths above and install with IFE's own FILE_SET.
target_sources (
    IrisFileExtensionLib INTERFACE FILE_SET HEADERS
    BASE_DIRS ${IFE_FILE_SET_BASE_DIRS} FILES
    ${IFE_SourcesExport}
)
target_sources (
    IrisFileExtensionLib PRIVATE
    # Headers first: the Xcode generator only puts files that appear in a
    # target's source list into the navigator, and FILE_SET HEADERS alone
    # (the install metadata above) is not enough. Headers are never compiled.
    ${IFE_SourcesExport}
    ${IFE_SourcesPriv}
)
target_include_directories (
    IrisFileExtensionLib PRIVATE
    ${IFE_IncludeDir}
)
# FetchContent consumers (Iris-Codec) include the PUBLIC headers —
# IrisFileExtension.hpp → generated IFE_Blocks.hpp — so
# src/ and generated_source/ must be on the INTERFACE path too. priv/ stays
# private on purpose: IrisMemory.hpp must not leak windows.h into consumers.
# $<BUILD_INTERFACE:...>: the paths are build-tree absolute and must not be
# exported to the install package (installed headers land flat in include/Iris).
target_include_directories (
    IrisFileExtensionLib INTERFACE
    $<BUILD_INTERFACE:${IFE_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${IFE_GENERATED_DIR}>
    # In-tree consumers need the two Iris-Headers headers the public surface
    # includes; installed ones get them flat from IFE's own FILE_SET below.
    $<BUILD_INTERFACE:${irisheaders_SOURCE_DIR}/include>
    # Installed headers land flat in include/Iris; the exported target must
    # say so, or a find_package consumer has no include path at all.
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Iris>
)
target_compile_definitions (
    IrisFileExtensionLib
    PRIVATE IRIS_EXPORT_API=true #Do not export Iris Core Fn-ality
    PRIVATE IFE_EXPORT_API=true 
    # Consumers in this tree link the objects or the static archive, never a
    # DLL import library. Left undefined, both macros expand to
    # __declspec(dllimport) on MSVC -- the export headers treat "not
    # exporting" and "importing" as the same case -- and every use emits an
    # __imp_ reference to a DLL this build never produces.
    INTERFACE IFE_EXPORT=
    INTERFACE IRIS_EXPORT=
)
set_target_properties(
    IrisFileExtensionLib 
    PROPERTIES 
    CXX_VISIBILITY_PRESET hidden
    # Required alongside the preset, not implied by it. GCC leaves weak
    # template and inline instantiations at default visibility even when the
    # preset is hidden, so std::optional<Iris::File::constants::...> lands in the
    # shared library's dynamic symbol table. Clang hides them already, which
    # is why this only ever failed on the Linux runners.
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
)
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# END Universal Object Build
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
set (
    IFE_Targets
    IrisFileExtensionLib
)
if(IFE_BUILD_SHARED AND NOT EMSCRIPTEN)
    # The wrapper's sources are IrisFileExtensionLib's objects, linked via the
    # object library (CMake >= 3.12) rather than listed through
    # $<TARGET_OBJECTS:...> in the source list — the Xcode generator emits no
    # Sources phase for either form, so no link is ever scheduled and the
    # target "builds" to nothing. IFE_Library.cpp is the one real compiled
    # source that gives the target a Sources phase and makes Xcode run the
    # link; its symbol is hidden and carries no ABI.
    add_library(IrisFileExtension SHARED ${IFE_SOURCE_DIR}/IFE_Library.cpp)
    target_link_libraries(IrisFileExtension PRIVATE IrisFileExtensionLib)
    target_include_directories(
        IrisFileExtension INTERFACE
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Iris>
    )
    set(IFE_Targets ${IFE_Targets} IrisFileExtension)
endif(IFE_BUILD_SHARED AND NOT EMSCRIPTEN)

if(IFE_BUILD_STATIC)
    add_library(IrisFileExtensionStatic STATIC ${IFE_SOURCE_DIR}/IFE_Library.cpp)
    target_link_libraries(IrisFileExtensionStatic PRIVATE IrisFileExtensionLib)
    target_include_directories(
        IrisFileExtensionStatic INTERFACE
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Iris>
    )
    set(IFE_Targets ${IFE_Targets} IrisFileExtensionStatic)
endif(IFE_BUILD_STATIC)

if (${PROJECT_IS_TOP_LEVEL})
    # Iris-Headers is a build-time dependency only. Its target is not exported
    # and its headers do not ship with this package: IFE's public surface
    # includes exactly IrisTypes.hpp and IrisCodecTypes.hpp, which install
    # with IFE's own FILE_SET below. Its runtime and codec headers
    # (IrisCore.hpp, IrisCodecCore.hpp) are downstream concerns and stay out
    # of IFE's install.
    install(
        TARGETS ${IFE_Targets}
        EXPORT IrisFileExtensionConfig
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Iris
    )
    install(
        EXPORT IrisFileExtensionConfig
        FILE IrisFileExtensionConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/IrisFileExtension
    )
endif(${PROJECT_IS_TOP_LEVEL})

if(IFE_BUILD_EXAMPLES)
    message(STATUS "Configured to build text example IFE implementations (/examples directory)")
    add_compile_definitions(BUILD_EXAMPLES_TEST)
    set(EXAMPLES_DIR ${PROJECT_SOURCE_DIR}/examples)

    add_executable(
        slide_info_abstraction 
        ${EXAMPLES_DIR}/slide_info_abstraction.cpp
    )
    # The example maps the slide through Iris::MemoryArena (priv/IrisMemory.hpp),
    # which lives in the object library rather than the exported shared surface —
    # so it links IrisFileExtensionLib (like the in-tree tests) and needs priv/
    # on its include path.
    target_include_directories (
        slide_info_abstraction PRIVATE 
        ${IFE_INCLUDE_DIR} ${irisheaders_SOURCE_DIR}/include
        ${irisheaders_SOURCE_DIR}/priv
    )  
    target_link_libraries(
        slide_info_abstraction PRIVATE 
        IrisFileExtensionLib ${IFE_Dependencies}
    )

    # The spec-conformance layer, attached and detached — a worked example
    # (see examples/validation_layer.cpp). The layer lives in the generated
    # IFE_Validation.{hpp,cpp}, deliberately OUTSIDE the library (see its
    # header comment): an application links it only when it wants it. This
    # compiles it alongside the generated layer, the way such an application
    # would.
    add_executable(
        ife_example_validation
        ${EXAMPLES_DIR}/validation_layer.cpp
        ${IFE_VALIDATION_SOURCE}
        ${IFE_GENERATED_SOURCES}
    )
    target_include_directories(
        ife_example_validation PRIVATE
        ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_example_validation PRIVATE cxx_std_20)
    target_link_libraries(
        ife_example_validation PRIVATE
        ${IFE_Dependencies}
    )
endif(IFE_BUILD_EXAMPLES)

# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# Tests — the whole suite lives in tests/tests.cmake
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
include(${CMAKE_CURRENT_SOURCE_DIR}/tests/tests.cmake)

# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# Export macros for in-tree executables
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# Every executable in this directory -- the examples and the whole test suite --
# compiles the runtime and the generated layer straight in, or links the object
# library; none links a DLL. (The IrisFileExtension* libraries are the
# exception: they keep IRIS_EXPORT_API=true so their own exports are unchanged.)
#
# IFE_EXPORT already tells "not exporting" from "importing", so it is correct on
# its own. IRIS_EXPORT is declared in Iris-Headers, which conflates the two:
# undefined, it becomes __declspec(dllimport) on MSVC, and every use of
# Iris::Result or Iris::Extent emits an __imp_ reference to a DLL this build
# never produces. Both headers guard with #ifndef, so defining them here wins.
#
# Read from the directory's real target list rather than a hand-kept one. The
# builder, parser, recovery, damage-sweep and advanced/api-contract tests were
# added without being appended to the old list, and the MSVC link failed on
# every one of them; a name list is a thing that goes stale silently.
get_property(_ife_directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_ife_target IN LISTS _ife_directory_targets)
    if(NOT _ife_target MATCHES "^IrisFileExtension")
        target_compile_definitions(${_ife_target} PRIVATE IFE_EXPORT= IRIS_EXPORT=)
    endif()
endforeach()

