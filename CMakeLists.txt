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
    FetchContent_Declare (
        IrisHeaders
        GIT_REPOSITORY https://github.com/IrisDigitalPathology/Iris-Headers.git
        GIT_TAG "origin/main"
        GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(IrisHeaders)
endif()

# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# Iris File Extension Universal Object Build
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
set (
    IFE_SourcesExport 
    ${IFE_INCLUDE_DIR}/IrisFileExtension.hpp
    ${IFE_INCLUDE_DIR}/IFE_Runtime.hpp
    ${IFE_INCLUDE_DIR}/IFE_Bytes.hpp
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
# IrisFileExtension.hpp → IFE_Runtime.hpp → generated IFE_Blocks.hpp — so
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
    # preset is hidden, so std::optional<IFE::constants::...> lands in the
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
    add_library(
        IrisFileExtension SHARED 
        $<TARGET_OBJECTS:IrisFileExtensionLib>
    ) 
    target_include_directories(
        IrisFileExtension INTERFACE
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Iris>
    )
    set(IFE_Targets ${IFE_Targets} IrisFileExtension)
endif(IFE_BUILD_SHARED AND NOT EMSCRIPTEN)

if(IFE_BUILD_STATIC)
    add_library(
        IrisFileExtensionStatic STATIC 
        $<TARGET_OBJECTS:IrisFileExtensionLib>
    )
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
# BEGIN Export macros for in-tree executables
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# Nothing here links a DLL. Tests and examples compile the runtime and the
# generated layer straight into the executable, or link the object library,
# so neither IFE_EXPORT nor IRIS_EXPORT should carry an attribute.
#
# IFE_EXPORT resolves correctly on its own (IFE_Runtime.hpp distinguishes
# "not exporting" from "importing"). IRIS_EXPORT is declared in Iris-Headers,
# which still conflates the two: undefined, it becomes __declspec(dllimport)
# on MSVC, and every use of Iris::Result or IrisCodec::AssociatedImageInfo
# emits an __imp_ reference to a DLL this build never produces. Both headers
# guard with #ifndef, so defining them here simply wins.
#
# Applied to the executables only. The library keeps IRIS_EXPORT_API=true so
# its own shared-object exports are unchanged.
foreach(target IN ITEMS
        ife_bytes_tests ife_blocks_tests ife_blocks_header_only_tests
        ife_window_tests ife_runtime_tests ife_lifetime_tests ife_corpus_tests
        ife_version_gating_tests ife_version_gating_backward_tests
        ife_validation_tests ife_v1_oracle_tests
        ife_example_runtime)
    if(TARGET ${target})
        target_compile_definitions(${target} PRIVATE IFE_EXPORT= IRIS_EXPORT=)
    endif()
endforeach()
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# END Export macros for in-tree executables
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
