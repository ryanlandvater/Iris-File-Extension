# The whole IFE test suite, extracted from CMakeLists.txt so the top-level
# build file stays readable. Included by CMakeLists.txt under the
# IFE_BUILD_TESTS guard; every variable it uses is defined there.
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

    # Scalar load/store primitives: the only place byte order and the packed
    # widths appear.  Header-only, so the test is the only consumer.
    add_executable(
        ife_bytes_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_bytes_tests.cpp
    )
    target_include_directories(ife_bytes_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR})
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
        ife_blocks_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
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
        ife_blocks_header_only_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_blocks_header_only_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_blocks_header_only_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_blocks_header_only_tests COMMAND ife_blocks_header_only_tests)

    # IFE_HEADER_ONLY across TWO translation units, linked together.
    #
    # The single-TU test above cannot see the failure that matters: if the
    # folded definitions lose their `inline`, one TU still links perfectly.
    # Two do not. This target is therefore mostly a link test, and it also
    # covers the circular include -- the header folds in the .cpp, the .cpp
    # opens by including the header -- from both directions.
    add_executable(
        ife_header_only_odr_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_header_only_odr_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/ife_header_only_odr_second.cpp
    )
    target_include_directories(
        ife_header_only_odr_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_header_only_odr_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_header_only_odr_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_header_only_odr_tests COMMAND ife_header_only_odr_tests)

    # Residency.  The remote mode exists for the browser; its transport is a
    # function pointer precisely so the cache and bounds logic can be driven
    # here, natively, with a stub.
    add_executable(
        ife_window_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_window_tests.cpp
        ${IFE_SOURCE_DIR}/IFE_Window.cpp
    )
    target_include_directories(ife_window_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR})
    target_compile_features(ife_window_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_window_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_window_tests COMMAND ife_window_tests)

    # The corpus: hosted fixtures pinned by digest in tests/corpus/manifest.json
    # (iris.exampleslides.org; never committed -- see tests/corpus/README.md).
    # Fetched at configure into the gitignored .deps/corpus/ so every test that
    # reads v1-written bytes has them before any binary runs. Unconditional
    # within test builds: the oracle and the >4 GiB test depend on the
    # snapshot, so -DIFE_CORPUS=OFF (a later opt-out for the corpus *test
    # target*) does not disable this fetch. Cached by digest, so an
    # already-fetched tree never touches the network.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch_corpus.py
                --manifest ${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/manifest.json
                --dest ${CMAKE_CURRENT_SOURCE_DIR}/.deps/corpus
        RESULT_VARIABLE IFE_CORPUS_RESULT
    )
    if(NOT IFE_CORPUS_RESULT EQUAL 0)
        message(FATAL_ERROR "IFE corpus fetch failed — see output above")
    endif()
    set(IFE_CORPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.deps/corpus)

    # The manifest, projected into a header the harness can read. This
    # repository has no JSON library and wants none, so the committed record
    # is generated into C++ at configure and gitignored — the same
    # arrangement as the version-gating fixture below.
    set(IFE_CORPUS_FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/corpus)
    execute_process(
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/corpus_manifest_header.py
                --manifest ${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/manifest.json
                --out ${IFE_CORPUS_FIXTURE_DIR}/corpus_manifest.hpp
        RESULT_VARIABLE IFE_CORPUS_HEADER_RESULT
    )
    if(NOT IFE_CORPUS_HEADER_RESULT EQUAL 0)
        message(FATAL_ERROR "IFE corpus manifest header generation failed")
    endif()

    # The corpus conformance gate: every fixture still holds what
    # tests/corpus/manifest.json claims it holds. Without this the corpus can
    # degrade silently — the other corpus tests read the bytes as a fixture
    # and never ask whether the coverage is still there.
    if(IFE_CORPUS)
        add_executable(
            ife_corpus_tests
            ${PROJECT_SOURCE_DIR}/tests/ife_corpus_tests.cpp
            ${IFE_SOURCE_DIR}/IFE_Runtime.cpp
            ${IFE_SOURCE_DIR}/IFE_Window.cpp
            ${IFE_GENERATED_SOURCES}
        )
        target_include_directories(
            ife_corpus_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
                                     ${IFE_CORPUS_FIXTURE_DIR}
        )
        target_compile_features(ife_corpus_tests PRIVATE cxx_std_20)
        target_link_libraries(ife_corpus_tests PRIVATE ${IFE_Dependencies})
        add_test(NAME ife_corpus_tests COMMAND ife_corpus_tests ${IFE_CORPUS_DIR})
        set_target_properties(ife_corpus_tests PROPERTIES FOLDER "Tests")
    endif()

    # The public API, end to end: the fetched snapshot in, the four entry
    # points Iris-Codec calls out. The snapshot lives in .deps/corpus/ (the
    # fetch above), so this test needs no second translation unit.
    add_executable(
        ife_runtime_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_runtime_tests.cpp
        ${IFE_SOURCE_DIR}/IFE_Runtime.cpp
        ${IFE_SOURCE_DIR}/IFE_Window.cpp
        ${IFE_GENERATED_SOURCES}
    )
    target_include_directories(
        ife_runtime_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_runtime_tests PRIVATE cxx_std_20)
    # The writer's path is a test ARGUMENT, not a compile definition. As a
    # definition it becomes a C string literal, and a Windows path does not
    # survive one: the compiler reads the backslashes as escape sequences.
    # CTest passes an argument through verbatim.
    # The corpus dir is a test ARGUMENT, not a compile definition. As a
    # definition it becomes a C string literal, and a Windows path does not
    # survive one: the compiler reads the backslashes as escape sequences.
    # CTest passes an argument through verbatim.
    target_link_libraries(ife_runtime_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_runtime_tests COMMAND ife_runtime_tests ${IFE_CORPUS_DIR})
    # A walk that forgets which nested attribute structures it has finished
    # with does not fail on a shared subtree, it stops returning. The timeout
    # is what turns that into a reported failure instead of a hung job.
    set_tests_properties(ife_runtime_tests PROPERTIES TIMEOUT 120)

    # No member of the abstraction may view into the mapping it was built
    # from (XP-4): the tree is built from a heap buffer, the buffer is
    # destroyed, and every string the tree keeps is read. Meaningless without
    # ASan — CI's build-asan job builds the whole suite with it.
    add_executable(
        ife_lifetime_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_lifetime_tests.cpp
        ${IFE_SOURCE_DIR}/IFE_Runtime.cpp
        ${IFE_SOURCE_DIR}/IFE_Window.cpp
        ${IFE_GENERATED_SOURCES}
    )
    target_include_directories(
        ife_lifetime_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_lifetime_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_lifetime_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_lifetime_tests COMMAND ife_lifetime_tests ${IFE_CORPUS_DIR})

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

    # Forward direction: the 200.0 build reads 1.0 and 200.0 files. The
    # fixture's block layer is header-only like the production one — nothing
    # to compile.
    # The fixture's own block source: the generated layer declares in the
    # header and defines in a translation unit, so a target consuming the
    # fixture header has to compile the fixture's .cpp (or define
    # IFE_HEADER_ONLY, which the sibling target below covers).
    add_executable(
        ife_version_gating_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_version_gating_tests.cpp
        ${IFE_FUTURE_FIXTURE_DIR}/generated/IFE_Blocks.cpp
    )
    target_include_directories(
        ife_version_gating_tests PRIVATE
        ${IFE_INCLUDE_DIR}
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
        ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
        ${IFE_FUTURE_FIXTURE_DIR}/generated
        ${IFE_FUTURE_FIXTURE_DIR}
    )
    target_compile_features(ife_version_gating_backward_tests PRIVATE cxx_std_20)
    target_link_libraries(ife_version_gating_backward_tests PRIVATE ${IFE_Dependencies})
    add_test(NAME ife_version_gating_backward_tests COMMAND ife_version_gating_backward_tests)

    add_executable(
        ife_example_runtime
        ${PROJECT_SOURCE_DIR}/examples/slide_info_abstraction.cpp
        ${IFE_SOURCE_DIR}/IFE_Runtime.cpp
        ${IFE_SOURCE_DIR}/IFE_Window.cpp
        ${IFE_GENERATED_SOURCES}
        # The example maps the slide through Iris::MemoryArena (same read-only
        # path as slide_info_abstraction). IFE_USE_RUNTIME must apply to this
        # target's own compile, so the arena source is compiled here rather
        # than pulled from the library.
        ${irisheaders_SOURCE_DIR}/src/IrisMemory.cpp
    )
    target_compile_definitions(ife_example_runtime PRIVATE IFE_USE_RUNTIME)
    target_include_directories(
        ife_example_runtime PRIVATE
        ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR} ${irisheaders_SOURCE_DIR}/priv
    )
    target_compile_features(ife_example_runtime PRIVATE cxx_std_20)
    target_link_libraries(ife_example_runtime PRIVATE ${IFE_Dependencies})

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
        ife_validation_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
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

    # Determinism (XP-3): the generator must be byte-identical run to run,
    # or --check and review diffs cannot be trusted. Stdlib-only, like the
    # generator; CI additionally runs it under PYTHONHASHSEED=0 and =1,
    # where dict/set iteration order is where non-determinism would enter.
    add_test(
        NAME ife_generator_determinism
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tests/generator/test_determinism.py
    )

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
        ${PROJECT_SOURCE_DIR}/tests/ife_v1_fixture.cpp
    )
    target_include_directories(
        ife_v1_oracle_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_v1_oracle_tests PRIVATE cxx_std_20)
    target_link_libraries(
        ife_v1_oracle_tests PRIVATE IrisFileExtensionLib ${IFE_Dependencies}
    )
    add_test(NAME ife_v1_oracle_tests COMMAND ife_v1_oracle_tests ${IFE_CORPUS_DIR})

    # The 1.1 witness: every 1.1 field asserted present-and-correct on
    # v1_1_witness.test_slide and absent on the 1.0 snapshot. The pair to the
    # v1 oracle, for the version-gated fields a 1.0 file cannot prove. Reads
    # the same fetched corpus; no generated sources here, for the same reason
    # as the oracle.
    add_executable(
        ife_v11_witness_tests
        ${PROJECT_SOURCE_DIR}/tests/ife_v11_witness_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/ife_v11_fixture.cpp
    )
    target_include_directories(
        ife_v11_witness_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_v11_witness_tests PRIVATE cxx_std_20)
    target_link_libraries(
        ife_v11_witness_tests PRIVATE IrisFileExtensionLib ${IFE_Dependencies}
    )
    add_test(NAME ife_v11_witness_tests COMMAND ife_v11_witness_tests ${IFE_CORPUS_DIR})

    # Produces the corpus snapshot the tests above read. Not a test, and
    # deliberately not run by the build: the snapshot is hosted and pinned by
    # digest, so regenerating it is an explicit act with an upload attached.
    #
    # Built against a *1.0-only* generated layer, derived from the committed
    # spec by build_baseline_spec.py. The reason is the oracle's substance: it
    # proves a 1.1 reader reads a 1.0 file, gating the newer fields off, and a
    # writer built from the committed spec can only emit 1.1 because store()
    # always lays out the newest version. The same derive-don't-copy
    # arrangement as the 200.0 fixture above, in the other direction.
    set(IFE_BASELINE_FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/baseline_versioning)
    execute_process(
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/build_baseline_spec.py
                ${CMAKE_CURRENT_SOURCE_DIR}/spec
                ${IFE_BASELINE_FIXTURE_DIR}
        RESULT_VARIABLE IFE_BASELINE_RESULT
    )
    if(NOT IFE_BASELINE_RESULT EQUAL 0)
        message(FATAL_ERROR "IFE baseline fixture spec builder failed — see output above")
    endif()
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -m generator
                --schema-dir ${IFE_BASELINE_FIXTURE_DIR}
                --out-dir ${IFE_BASELINE_FIXTURE_DIR}/generated
                --docs-dir ${IFE_BASELINE_FIXTURE_DIR}/generated_docs
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE IFE_BASELINE_GEN_RESULT
    )
    if(NOT IFE_BASELINE_GEN_RESULT EQUAL 0)
        message(FATAL_ERROR "IFE baseline fixture generation failed — see output above")
    endif()

    # EXCLUDE_FROM_ALL: nothing depends on it and it is run by hand a few
    # times a year, so `cmake --build .` should not compile it. Build it
    # explicitly when regenerating the snapshot:
    #     cmake --build build --target ife_snapshot_writer
    # The 1.1 corpus fixture writer. Built against the CURRENT generated
    # layer, unlike ife_snapshot_writer below, which is deliberately built
    # against the 1.0 baseline: 1.1 content cannot come out of a 1.0 schema,
    # and the snapshot's content is pinned to the oracle's expectations.
    # EXCLUDE_FROM_ALL for the same reason as the snapshot writer -- run by
    # hand when a fixture is (re)generated:
    #     cmake --build build --target ife_corpus_writer_11
    add_executable(
        ife_corpus_writer_11 EXCLUDE_FROM_ALL
        ${PROJECT_SOURCE_DIR}/tests/ife_corpus_writer_11.cpp
        ${IFE_GENERATED_SOURCES}
    )
    target_include_directories(
        ife_corpus_writer_11 PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
    )
    target_compile_features(ife_corpus_writer_11 PRIVATE cxx_std_20)
    target_link_libraries(ife_corpus_writer_11 PRIVATE ${IFE_Dependencies})
    set_target_properties(ife_corpus_writer_11 PROPERTIES FOLDER "Tests")

    add_executable(
        ife_snapshot_writer EXCLUDE_FROM_ALL
        ${PROJECT_SOURCE_DIR}/tests/ife_snapshot_writer.cpp
        # The BASELINE block layer, not ${IFE_GENERATED_SOURCES}: this writer
        # emits 1.0 bytes and must link the 1.0 store()s that go with the 1.0
        # headers it includes. Without it the target does not link at all --
        # it is EXCLUDE_FROM_ALL, so that went unnoticed.
        ${IFE_BASELINE_FIXTURE_DIR}/generated/IFE_Blocks.cpp
    )
    target_include_directories(
        ife_snapshot_writer PRIVATE ${IFE_INCLUDE_DIR}
        ${IFE_BASELINE_FIXTURE_DIR}/generated
        ${PROJECT_SOURCE_DIR}/tests
    )
    target_compile_features(ife_snapshot_writer PRIVATE cxx_std_20)
    target_link_libraries(ife_snapshot_writer PRIVATE ${IFE_Dependencies})

    # A whole slide larger than 4 GiB, so the tile offsets a reader follows
    # need their fifth byte.  Sparse: over 4 GiB long, tens of kilobytes on
    # disk.
    #
    # Not on a 32-bit host: a 32-bit address space cannot map the file at all.
    # Windows is fine -- the test maps through Iris::MemoryArena
    # (priv/IrisMemory.hpp), which marks NTFS files sparse with
    # FSCTL_SET_SPARSE before mapping, so the file stays sparse there too.
    # The u40 primitives themselves are covered everywhere by ife_bytes_tests
    # and ife_blocks_tests -- what only this test reaches is a complete,
    # validated file whose offsets exceed 32 bits.
    if (CMAKE_SIZEOF_VOID_P GREATER_EQUAL 8)
        add_executable(
            ife_large_file_tests
            ${PROJECT_SOURCE_DIR}/tests/ife_large_file_tests.cpp
        )
        # priv/ as well: the test's SparseFile maps through Iris::MemoryArena,
        # declared in IrisMemory.hpp there.
        target_include_directories(
            ife_large_file_tests PRIVATE ${IFE_INCLUDE_DIR} ${IFE_GENERATED_DIR}
                                        ${irisheaders_SOURCE_DIR}/priv
        )
        target_compile_features(ife_large_file_tests PRIVATE cxx_std_20)
        target_link_libraries(
            ife_large_file_tests PRIVATE IrisFileExtensionLib ${IFE_Dependencies}
        )
        # The scratch directory is an argument, not a compile definition: a
        # definition becomes a C string literal and a path does not survive one.
        add_test(NAME ife_large_file_tests
                 COMMAND ife_large_file_tests $<TARGET_FILE_DIR:ife_large_file_tests>
                                              ${IFE_CORPUS_DIR})
        set_target_properties(ife_large_file_tests PROPERTIES FOLDER "Tests")
    endif()

    # Xcode/VS navigator hygiene: the test targets (and the slide-writer
    # fixture tool) go under one "Tests" folder instead of a top-level entry
    # each. The libraries and the example stay top-level.
    foreach(t ife_bytes_tests ife_blocks_tests
              ife_blocks_header_only_tests ife_window_tests
              ife_runtime_tests ife_lifetime_tests ife_version_gating_tests
              ife_version_gating_backward_tests ife_example_runtime
              ife_validation_tests ife_v1_oracle_tests ife_v11_witness_tests)
        set_target_properties(${t} PROPERTIES FOLDER "Tests")
    endforeach()

endif()