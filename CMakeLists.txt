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
option(IFE_BUILD_TESTS "Build IFE unit/stress tests" OFF)
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
    ${IFE_SOURCE_DIR}/IFE_Bytes.hpp
    ${IFE_SOURCE_DIR}/IFE_Window.hpp
)
# IFE_Runtime.{hpp,cpp} is deliberately NOT in the library above.  It defines the
# same four IrisCodec entry points as the hand-written layer -- that is the whole
# point, so a consumer switches by changing one include -- and two definitions
# cannot coexist in one binary.  Retiring the hand-written layer removes
# IrisCodecExtension.* and moves these two lines up.  Until then the runtime
# builds into its test targets only.
set(IFE_RuntimeSources
    ${IFE_SOURCE_DIR}/IFE_Runtime.hpp
    ${IFE_SOURCE_DIR}/IFE_Runtime.cpp
)
set (
    IFE_SourcesPriv
    ${IFE_SOURCE_DIR}/IrisCodecExtension.cpp
    ${IFE_SOURCE_DIR}/IFE_Window.cpp
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
# The conformance layer is a development tool, not a production
# dependency: it ships as its own target so an application links
# it only when it wants validation.  Keep it out of the library's sources.
set(IFE_VALIDATION_SOURCE ${IFE_GENERATED_DIR}/IFE_Validation.cpp)
list(REMOVE_ITEM IFE_GENERATED_SOURCES ${IFE_VALIDATION_SOURCE})
file(GLOB IFE_GENERATED_HEADERS CONFIGURE_DEPENDS ${IFE_GENERATED_DIR}/*.hpp)
if(IFE_GENERATED_SOURCES OR IFE_GENERATED_HEADERS)
    list(APPEND IFE_SourcesPriv   ${IFE_GENERATED_SOURCES})
    list(APPEND IFE_SourcesExport ${IFE_GENERATED_HEADERS})
    list(APPEND IFE_IncludeDir    ${IFE_GENERATED_DIR})
    message(STATUS "IFE: compiling generated sources from ${IFE_GENERATED_DIR}")
endif()
# Generated headers live under their own base dir for the FILE_SET; keep
# the base list conditional so an empty generated_source/ (before the first
# generation) never references a nonexistent directory.
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

    # Every test binary builds into its own tree — <binary>/cm/tests — so the
    # build root stays a map of the library artifacts instead of a pile of
    # executables. The libraries and the standalone example are declared
    # above this point, so nothing else is swept along. The path is relative
    # to the binary dir itself, whatever it is named (-B build/cm gives
    # build/cm/tests). Multi-config generators (Xcode, Visual Studio) append
    # the configuration subdirectory: tests/Debug.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")

    # Wire-format parity: compile-time proof that the generated layout equals
    # shipped IFE 1.0.  Pure static_assert - building it IS the test, so it is
    # registered only so `ctest` reports it.  Retired with the hand-written
    # layer, when it is retired.
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

    # Generated block handles: reading and validation executed against a real
    # byte buffer, which compiling the header cannot tell you.
    add_executable(
        ife_blocks_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_blocks_tests.cpp
        ${IFE_GENERATED_SOURCES}
    )
    target_include_directories(
        ife_blocks_tests PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_blocks_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_blocks_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_blocks_tests COMMAND ife_blocks_tests)

    # The same test compiled with IFE_HEADER_ONLY, so both ways of consuming
    # the generated layer are covered by the build rather than by assertion.
    add_executable(
        ife_blocks_header_only_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_blocks_tests.cpp
    )
    target_compile_definitions(ife_blocks_header_only_tests PRIVATE IFE_HEADER_ONLY)
    target_include_directories(
        ife_blocks_header_only_tests PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_blocks_header_only_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_blocks_header_only_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_blocks_header_only_tests COMMAND ife_blocks_header_only_tests)

    # Residency.  The remote mode exists for the browser; its transport is a
    # function pointer precisely so the cache and bounds logic can be driven
    # here, natively, with a stub.
    add_executable(
        ife_window_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_window_tests.cpp
        ${IFE_SOURCE_DIR}/IFE_Window.cpp
    )
    target_include_directories(ife_window_tests PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR})
    target_compile_features(ife_window_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_window_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_window_tests COMMAND ife_window_tests)

    # The public API, end to end: v1 encodes in ife_v1_fixture.cpp, the
    # generated stack reads through the four entry points Iris-Codec calls.
    # Two translation units because IFE_Runtime.hpp and IrisCodecExtension.hpp
    # are mutually exclusive -- which is the property that makes the cutover
    # a one-line include change.
    # A slide written by the SHIPPED encoder, as a standalone tool.  It links
    # the hand-written layer; the runtime test does not, and cannot -- so the
    # file passes between them on disk, exactly as it would for a real consumer.
    add_executable(
        ife_v1_slide_writer
        ${PROJECT_SOURCE_DIR}/tests/ife_v1_slide_writer.cpp
        ${PROJECT_SOURCE_DIR}/tests/ife_v1_fixture.cpp
    )
    target_include_directories(
        ife_v1_slide_writer PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_v1_slide_writer PRIVATE cxx_std_20)
    target_link_libraries(
        ife_v1_slide_writer PRIVATE IrisFileExtensionLib ${IFE_Dependencies}
    )

    add_executable(
        ife_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_runtime_tests.cpp
        ${IFE_SOURCE_DIR}/IFE_Runtime.cpp
        ${IFE_SOURCE_DIR}/IFE_Window.cpp
        ${IFE_GENERATED_SOURCES}
    )
    target_include_directories(
        ife_runtime_tests PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_runtime_tests PRIVATE cxx_std_20)
    target_compile_definitions(
        ife_runtime_tests PRIVATE
        IFE_V1_SLIDE_WRITER="$<TARGET_FILE:ife_v1_slide_writer>"
    )
    add_dependencies(ife_runtime_tests ife_v1_slide_writer)
    target_link_libraries(ife_runtime_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_runtime_tests COMMAND ife_runtime_tests)

    # Version gating, executed. The real spec is 1.0-only, so the
    # since-mechanism — optional accessors gated by the file version and the
    # stored stride — compiles but never runs. A derived fixture spec
    # (tests/fixtures/build_future_spec.py) adds two fake fields and bumps
    # the version to 200.0; the generated layer built from it is what the
    # two version-gating tests below compile against. The version is extreme
    # on purpose: it proves the comparison is numeric, and stays a valid
    # future version for decades.
    #
    # The fixture lives under the test suite (tests/fixtures/future_versioning/),
    # generated at configure time and gitignored — the same arrangement as
    # generated_source/.
    set(IFE_FUTURE_FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/future_versioning)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/build_future_spec.py
                ${CMAKE_CURRENT_SOURCE_DIR}/spec
                ${IFE_FUTURE_FIXTURE_DIR}
        RESULT_VARIABLE IFE_FIXTURE_RESULT
    )
    if(NOT IFE_FIXTURE_RESULT EQUAL 0)
        message(FATAL_ERROR "IFE fixture spec builder failed — see output above")
    endif()
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -m generator
                --schema-dir ${IFE_FUTURE_FIXTURE_DIR}
                --out-dir ${IFE_FUTURE_FIXTURE_DIR}/generated
                --docs-dir ${IFE_FUTURE_FIXTURE_DIR}/generated_docs
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE IFE_FIXTURE_GEN_RESULT
    )
    if(NOT IFE_FIXTURE_GEN_RESULT EQUAL 0)
        message(FATAL_ERROR "IFE future-version fixture generation failed — see output above")
    endif()

    # Forward direction: the 200.0 build reads 1.0 and 200.0 files.
    add_executable(
        ife_version_gating_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_version_gating_tests.cpp
        ${IFE_FUTURE_FIXTURE_DIR}/generated/IFE_Blocks.cpp
    )
    target_include_directories(
        ife_version_gating_tests PRIVATE
        ${IFE_SOURCE_DIR}
        ${IFE_FUTURE_FIXTURE_DIR}/generated
        ${IFE_FUTURE_FIXTURE_DIR}
    )
    target_compile_features(ife_version_gating_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_version_gating_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_version_gating_tests COMMAND ife_version_gating_tests)

    # Backward direction: the production 1.0 build reads the same 200.0 file.
    add_executable(
        ife_version_gating_backward_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_version_gating_backward_tests.cpp
        ${IFE_GENERATED_SOURCES}
    )
    target_include_directories(
        ife_version_gating_backward_tests PRIVATE
        ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR}
        ${IFE_FUTURE_FIXTURE_DIR}/generated
        ${IFE_FUTURE_FIXTURE_DIR}
    )
    target_compile_features(ife_version_gating_backward_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_version_gating_backward_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_version_gating_backward_tests COMMAND ife_version_gating_backward_tests)

    # The runtime acceptance criterion, made executable: the same example source
    # compiled against each layer, run on the same slide, and diffed.  If the
    # two outputs differ, the port changed observable behaviour.
    add_executable(
        ife_example_v1 ${PROJECT_SOURCE_DIR}/examples/slide_info_abstraction.cpp
    )
    target_compile_definitions(ife_example_v1 PRIVATE BUILD_EXAMPLES_TEST)
    target_include_directories(ife_example_v1 PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR})
    target_compile_features(ife_example_v1 PRIVATE cxx_std_20)
    target_link_libraries(ife_example_v1 PRIVATE IrisFileExtensionLib ${IFE_Dependencies})

    add_executable(
        ife_example_runtime
        ${PROJECT_SOURCE_DIR}/examples/slide_info_abstraction.cpp
        ${IFE_SOURCE_DIR}/IFE_Runtime.cpp
        ${IFE_SOURCE_DIR}/IFE_Window.cpp
        ${IFE_GENERATED_SOURCES}
    )
    target_compile_definitions(ife_example_runtime PRIVATE IFE_USE_RUNTIME)
    target_include_directories(ife_example_runtime PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR})
    target_compile_features(ife_example_runtime PRIVATE cxx_std_20)
    target_link_libraries(ife_example_runtime PRIVATE ${IFE_Dependencies})

    add_test(
        NAME ife_example_parity
        COMMAND ${CMAKE_COMMAND}
            -DWRITER=$<TARGET_FILE:ife_v1_slide_writer>
            -DEXAMPLE_V1=$<TARGET_FILE:ife_example_v1>
            -DEXAMPLE_RUNTIME=$<TARGET_FILE:ife_example_runtime>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}
            -P ${PROJECT_SOURCE_DIR}/tests/example_parity.cmake
    )

    # The conformance layer, attached and detached.  Both paths asserted,
    # because a layer that is never consulted is the failure mode a dispatch
    # point has, and a check that fires when detached would make the "costs a
    # null check" claim false.
    add_executable(
        ife_validation_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_validation_tests.cpp
        ${IFE_VALIDATION_SOURCE}
        ${IFE_GENERATED_SOURCES}
    )
    target_include_directories(
        ife_validation_tests PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_validation_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_validation_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_validation_tests COMMAND ife_validation_tests)

    # The generated layer stays out of the exported ABI.
    if(IFE_BUILD_SHARED AND NOT EMSCRIPTEN)
        add_test(
            NAME ife_exported_symbols
            COMMAND ${CMAKE_COMMAND}
                -DLIBRARY=$<TARGET_FILE:IrisFileExtension>
                -P ${PROJECT_SOURCE_DIR}/tests/exported_symbols.cmake
        )
    endif()

    # The v1 oracle: bytes written by the SHIPPED encoder, read back through
    # the generated layer.  The only gate that compares the new reader against
    # real output rather than against another description of the format, which
    # is why it links IrisFileExtensionLib - it needs v1's STORE_* to run, not
    # merely its headers.  Retired with the hand-written layer, and
    # not one commit sooner: an oracle cannot be deleted before what it gates.
    # No ${IFE_GENERATED_SOURCES} here: IrisFileExtensionLib already compiles
    # them, and listing them again gives every generated definition twice.
    add_executable(
        ife_v1_oracle_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_v1_oracle_tests.cpp
    )
    target_include_directories(
        ife_v1_oracle_tests PRIVATE ${IFE_SOURCE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_v1_oracle_tests PRIVATE cxx_std_20)
    target_link_libraries(
        ife_v1_oracle_tests PRIVATE IrisFileExtensionLib ${IFE_Dependencies}
    )
    add_test(NAME ife_v1_oracle_tests COMMAND ife_v1_oracle_tests)

    # Xcode/VS navigator hygiene: the test targets (and the slide-writer
    # fixture tool) go under one "Tests" folder instead of a top-level entry
    # each. The libraries and the example stay top-level.
    foreach(t ife_wire_parity_tests ife_bytes_tests ife_blocks_tests
              ife_blocks_header_only_tests ife_window_tests ife_v1_slide_writer
              ife_runtime_tests ife_version_gating_tests
              ife_version_gating_backward_tests ife_example_v1 ife_example_runtime
              ife_validation_tests ife_v1_oracle_tests)
        set_target_properties(${t} PROPERTIES FOLDER "Tests")
    endforeach()

endif()

