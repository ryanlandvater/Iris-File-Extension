# CMake for Iris File Extension
# 2025 Copyright Ryan Landvater
# Created 3/3/2025

cmake_minimum_required(VERSION 3.11...3.30)
include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(IRIS_EXTENSION_BUILD_ALL ON)
# set(STATIC_BUILD OFF)

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

if ((MSVC) AND (MSVC_VERSION GREATER_EQUAL 1914))
    target_compile_options(stxxl INTERFACE "/Zc:__cplusplus")
endif()

Project(
    IrisFileExtension 
    LANGUAGES CXX 
    VERSION ${VERSION_STRING}
)
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

add_library(IrisFileExtensionLib OBJECT)
target_sources (
    IrisFileExtensionLib INTERFACE FILE_SET HEADERS
    BASE_DIRS ${IFE_SOURCE_DIR}
    FILES
    ${IFE_SOURCE_DIR}/IrisFileExtension.hpp
    ${IFE_SOURCE_DIR}/IrisCodecExtension.hpp
)
target_sources (
    IrisFileExtensionLib PRIVATE
    ${IFE_SOURCE_DIR}/IrisCodecExtension.cpp
    ${irisheaders_SOURCE_DIR}/src/IrisBuffer.cpp
)
target_include_directories (
    IrisFileExtensionLib INTERFACE
    $<BUILD_INTERFACE:${IFE_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries (
    IrisFileExtensionLib PRIVATE
    IrisHeaders
)
add_library(IrisFileExtension SHARED $<TARGET_OBJECTS:IrisFileExtensionLib>)
add_library(IrisFileExtensionStatic STATIC $<TARGET_OBJECTS:IrisFileExtensionLib>)
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
        TARGETS IrisFileExtensionLib IrisFileExtension IrisFileExtensionStatic
        EXPORT IrisFileExtensionConfig
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Iris
    )
    install(
        EXPORT IrisFileExtensionConfig
        FILE IrisFileExtensionConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Iris
    )
endif()