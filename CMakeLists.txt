# CMake for Iris File Extension
# 2025 Copyright Ryan Landvater
# Created 3/3/2025

cmake_minimum_required(VERSION 3.11...3.30)
include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(IFE_get_version)
    set(IFE_core_header "${CMAKE_CURRENT_SOURCE_DIR}/src/IrisCodecExtension.hpp")
    if (NOT EXISTS ${IFE_core_header})
        message(FATAL_ERROR "Couldn't find IrisCodecExtension.hpp!")
    endif()

    file(READ ${IFE_core_header} header)

    if (header MATCHES "uint16_t[ ]*IRIS_EXTENSION_MAJOR[ ]*=[ ]*([0-9])")
        set(MAJOR_VERSION "${CMAKE_MATCH_1}")
    else () 
        message(FATAL_ERROR "Couldn't get the IFE major Version")
    endif()

    if (header MATCHES "uint16_t[ ]*IRIS_EXTENSION_MINOR[ ]*=[ ]*([0-9])")
        set(MINOR_VERSION "${CMAKE_MATCH_1}")
    else () 
        message(FATAL_ERROR "Couldn't get the IFE minor version")
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
option(IFE_BUILD_TESTS "Build IFE unit/stress tests (requires IFE_USE_FASTFHIR_SUBSTRATE)" OFF)
# Phase 1 of the FastFHIR substrate migration. Dormant when OFF; enabling it
# compiles the new lock-free VMA layer alongside the legacy Abstraction::File
# code. Phases 2-6 progressively replace the legacy paths.
option(IFE_USE_FASTFHIR_SUBSTRATE "Compile the new FastFHIR-style substrate (IFE_Memory, etc.)" OFF)
option(IFE_RUN_GENERATOR "Regenerate generated_source/ at configure (requires generator/)" OFF)
if(IRIS_EXTENSION_BUILD_ALL)
    set (IFE_BUILD_SHARED ON)
    set (IFE_BUILD_STATIC ON)
    set (IFE_BUILD_EXAMPLES ON)
endif(IRIS_EXTENSION_BUILD_ALL)

set(IFE_SOURCE_DIR ${PROJECT_SOURCE_DIR}/src)

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
    ${IFE_SOURCE_DIR}/IrisFileExtension.hpp
    ${IFE_SOURCE_DIR}/IrisCodecExtension.hpp
)
set (
    IFE_SourcesPriv
    ${IFE_SOURCE_DIR}/IrisCodecExtension.cpp
    ${irisheaders_SOURCE_DIR}/src/IrisBuffer.cpp
)
if(IFE_USE_FASTFHIR_SUBSTRATE)
    list(APPEND IFE_SourcesExport ${IFE_SOURCE_DIR}/IFE_Memory.hpp)
    list(APPEND IFE_SourcesPriv   ${IFE_SOURCE_DIR}/IFE_Memory.cpp)
    message(STATUS "IFE: FastFHIR substrate ENABLED (Phase 1: IFE_Memory)")
endif()
set (
    IFE_IncludeDir
    ${irisheaders_SOURCE_DIR}/priv
    ${IFE_SOURCE_DIR}
)
set (
    IFE_Dependencies
    IrisHeaders
)
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# Generated source (generated_source/) and generated docs
# (generated_docs/) — Phase 3 pipeline output. Both are gitignored and
# freely regenerable; the committed spec JSON is the source of truth.
# Generation runs only when the pipeline exists, and only then if
# IFE_RUN_GENERATOR=ON or the anchor file is missing.
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
set(IFE_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_source")
set(IFE_DOCS_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/generated_docs")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/generator/pipeline.py)
    if(IFE_RUN_GENERATOR OR NOT EXISTS "${IFE_GENERATED_DIR}/IFE_VTables.hpp")
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
file(GLOB IFE_GENERATED_HEADERS CONFIGURE_DEPENDS ${IFE_GENERATED_DIR}/*.hpp)
if(IFE_GENERATED_SOURCES OR IFE_GENERATED_HEADERS)
    list(APPEND IFE_SourcesPriv   ${IFE_GENERATED_SOURCES})
    list(APPEND IFE_SourcesExport ${IFE_GENERATED_HEADERS})
    list(APPEND IFE_IncludeDir    ${IFE_GENERATED_DIR})
    message(STATUS "IFE: compiling generated sources from ${IFE_GENERATED_DIR}")
endif()
# Generated headers live under their own base dir for the FILE_SET; keep
# the base list conditional so an empty generated_source/ (pre-Phase 3)
# never references a nonexistent directory.
set(IFE_FILE_SET_BASE_DIRS ${IFE_SOURCE_DIR})
if(IFE_GENERATED_HEADERS)
    list(APPEND IFE_FILE_SET_BASE_DIRS ${IFE_GENERATED_DIR})
endif()
add_library(IrisFileExtensionLib OBJECT)
target_sources (
    IrisFileExtensionLib INTERFACE FILE_SET HEADERS
    BASE_DIRS ${IFE_FILE_SET_BASE_DIRS} FILES
    ${IFE_SourcesExport}
)
target_sources (
    IrisFileExtensionLib PRIVATE
    ${IFE_SourcesPriv}
)
target_include_directories (
    IrisFileExtensionLib PRIVATE
    ${IFE_IncludeDir}
)
target_compile_definitions (
    IrisFileExtensionLib
    PRIVATE IRIS_EXPORT_API=true #Do not export Iris Core Fn-ality
    PRIVATE IFE_EXPORT_API=true 
)
set_target_properties(
    IrisFileExtensionLib 
    PROPERTIES CXX_VISIBILITY_PRESET hidden
)
target_link_libraries (
    IrisFileExtensionLib
    PRIVATE ${IFE_Dependencies}
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
    target_link_libraries(
        IrisFileExtension
        PRIVATE ${IFE_Dependencies}
    )
    set(IFE_Targets ${IFE_Targets} IrisFileExtension)
endif(IFE_BUILD_SHARED AND NOT EMSCRIPTEN)

if(IFE_BUILD_STATIC)
    add_library(
        IrisFileExtensionStatic STATIC 
        $<TARGET_OBJECTS:IrisFileExtensionLib>
    )
    target_link_libraries (
        IrisFileExtensionStatic
        PRIVATE ${IFE_Dependencies}
    )
    set(IFE_Targets ${IFE_Targets} IrisFileExtensionStatic)
endif(IFE_BUILD_STATIC)

if (${PROJECT_IS_TOP_LEVEL})
    install(
        TARGETS IrisHeaders
        EXPORT IrisHeadersConfig
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Iris
    )
    install(
        EXPORT IrisHeadersConfig
        FILE IrisHeadersConfig.cmake
        NAMESPACE "Iris::"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Iris
    )
    install(
        TARGETS ${IFE_Targets}
        EXPORT IrisFileExtensionConfig
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Iris
    )
    install(
        EXPORT IrisFileExtensionConfig
        FILE IrisFileExtensionConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Iris
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
    target_include_directories (
        slide_info_abstraction PRIVATE 
        ${IFE_SOURCE_DIR} ${irisheaders_SOURCE_DIR}/include
    )  
    target_link_libraries(
        slide_info_abstraction PUBLIC 
        IrisFileExtension
    )
endif(IFE_BUILD_EXAMPLES)

# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# Tests
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
if(IFE_BUILD_TESTS)
    enable_testing()

    # Wire-format parity: compile-time proof that the generated layout equals
    # shipped IFE 1.0.  Pure static_assert - building it IS the test, so it is
    # registered only so `ctest` reports it.  Retired with the hand-written
    # layer in Phase 6.
    add_executable(
        ife_wire_parity_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_wire_parity_tests.cpp
    )
    target_include_directories(
        ife_wire_parity_tests PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_wire_parity_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_wire_parity_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_wire_parity_tests COMMAND ife_wire_parity_tests)

    # Scalar load/store primitives: the only place byte order and the packed
    # widths appear.  Header-only, so the test is the only consumer.
    add_executable(
        ife_bytes_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_bytes_tests.cpp
    )
    target_include_directories(ife_bytes_tests PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR})
    target_compile_features(ife_bytes_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_bytes_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_bytes_tests COMMAND ife_bytes_tests)

    # The IFE_Memory substrate is dormant and scheduled for deletion at Phase 4
    # exit; its tests build only when it does.  This requirement belongs to the
    # target, not to IFE_BUILD_TESTS as a whole - the generated-layer tests do
    # not depend on the substrate.
    if(IFE_USE_FASTFHIR_SUBSTRATE)
        add_executable(
            ife_memory_tests
            ${PROJECT_SOURCE_DIR}/tests/ife_memory_tests.cpp
            ${IFE_SOURCE_DIR}/IFE_Memory.cpp
        )
        target_include_directories(ife_memory_tests PRIVATE ${IFE_SOURCE_DIR})
        target_compile_features(ife_memory_tests PRIVATE cxx_std_20)
        find_package(Threads REQUIRED)
        target_link_libraries(ife_memory_tests PRIVATE Threads::Threads)
        add_test(NAME ife_memory_tests COMMAND ife_memory_tests)
    endif()
endif()
