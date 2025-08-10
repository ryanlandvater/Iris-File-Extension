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
