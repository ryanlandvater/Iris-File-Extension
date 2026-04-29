# CMake for Iris File Extension
# 2025 Copyright Ryan Landvater
# Created 3/3/2025

cmake_minimum_required(VERSION 3.11...3.30)
include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Always emit position-independent code. The IrisFileExtensionLib OBJECT
# library is linked into both the static and shared library targets, so its
# objects must be PIC even when consumers only build the static archive
# (otherwise the shared-library link fails on platforms that require PIC,
# e.g. x86_64 Linux). Setting it globally also covers the test executables
# and any future targets.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
if(IRIS_EXTENSION_BUILD_ALL)
    set (IFE_BUILD_SHARED ON)
    set (IFE_BUILD_STATIC ON)
    set (IFE_BUILD_EXAMPLES ON)
endif(IRIS_EXTENSION_BUILD_ALL)

set(IFE_SOURCE_DIR ${PROJECT_SOURCE_DIR}/src)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if (NOT DEFINED irisheaders_SOURCE_DIR)
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
    list(APPEND IFE_SourcesExport ${IFE_SOURCE_DIR}/IFE_Types.hpp)
    list(APPEND IFE_SourcesPriv   ${IFE_SOURCE_DIR}/IFE_Memory.cpp)
    message(STATUS "IFE: FastFHIR substrate ENABLED (Phases 1-2: IFE_Memory, IFE_Types)")
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
add_library(IrisFileExtensionLib OBJECT)
target_sources (
    IrisFileExtensionLib INTERFACE FILE_SET HEADERS
    BASE_DIRS ${IFE_SOURCE_DIR} FILES
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
# Tests (Phase 1 substrate)
# ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
if(IFE_BUILD_TESTS)
    if(NOT IFE_USE_FASTFHIR_SUBSTRATE)
        message(FATAL_ERROR "IFE_BUILD_TESTS requires IFE_USE_FASTFHIR_SUBSTRATE=ON")
    endif()
    enable_testing()
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

    # Phase 2 — IFE_Types header. Pulls in IrisCodecExtension.hpp for the
    # legacy<->tag round-trip pinning, so it needs the same include paths
    # (IrisHeaders + IFE source) as the main library.
    add_executable(
        ife_types_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_types_tests.cpp
    )
    target_include_directories(ife_types_tests PRIVATE ${IFE_IncludeDir})
    target_compile_features(ife_types_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_types_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_types_tests COMMAND ife_types_tests)
endif()
