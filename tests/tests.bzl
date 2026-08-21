# tests/tests.bzl — the IFE test suite, declared from one macro.
#
# Everything `bazel test //...` needs: the future-version fixture genrules,
# the header-only input library and the twelve cc_test targets. They live
# here rather than inline in BUILD.bazel so the root BUILD file reads as the
# library graph plus one call; the macro expands in the CALLING package (the
# root), so relative labels like "tests/ife_bytes_tests.cpp" and the fixture
# paths under tests/fixtures/future_versioning/ resolve exactly as they did
# inline — a rule's outputs must live in its own package, which is why the
# fixture genrules cannot move into a tests/ subpackage.
#
# The macro is parameterised on everything the root package owns — the copts
# select, the header filegroups and the two libraries — and hardcodes the
# external deps (iris_headers, runfiles), which are not this repo's to
# relabel. It is only ever called from the root BUILD.bazel.
load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")


def ife_tests(
        copts,
        src_headers,
        generated_headers,
        ife,
        ife_validation,
        python_resolve):
    """Declare the full test suite in the calling (root) package.

    Args:
        copts: the platform compile-flags select (windows /std:c++20).
        src_headers: label of the root filegroup of hand-written headers.
        generated_headers: label of the root filegroup of generated headers.
        ife: label of the core library.
        ife_validation: label of the opt-in validation library.
        python_resolve: the PYTHON_RESOLVE interpreter-guard string, for the
            fixture generator genrule.
    """

    # ── Future-version fixture (version-gating tests) ────────────────────
    # The real spec is 1.1-only, so the since-mechanism compiles but never
    # runs. tests/fixtures/build_future_spec.py injects two fields and bumps
    # the version to 200.0; the layer generated from it is what the gating
    # tests compile against (CMake does the same at configure time). Both
    # genrules live in the root package, so they share $(RULEDIR) and the
    # second can name the first's output directory directly.
    # ── Corpus manifest header (corpus conformance test) ─────────────────
    # tests/corpus/manifest.json is the committed record of what the corpus
    # contains; the harness needs it in C++, and this repo has no JSON
    # library. Projected into a header here exactly as CMake does at
    # configure. Output lands in the root package, like the fixture genrules
    # below, because a rule's outputs must live in its own package.
    native.genrule(
        name = "corpus_manifest_header",
        srcs = [
            "tools/corpus_manifest_header.py",
            "//tests/corpus:manifest.json",
        ],
        outs = ["tests/fixtures/corpus/corpus_manifest.hpp"],
        cmd = python_resolve +
              "$$PY $(location tools/corpus_manifest_header.py) " +
              "--manifest $(location //tests/corpus:manifest.json) " +
              "--out $@",
    )

    # The corpus conformance gate: every fixture still holds what the
    # manifest claims. The other corpus tests read the bytes as a fixture and
    # would stay green through a fixture that quietly lost its annotations.
    cc_test(
        name = "ife_corpus_tests",
        srcs = [
            "tests/ife_corpus_tests.cpp",
            "tests/ife_corpus_path.hpp",
            "tests/fixtures/corpus/corpus_manifest.hpp",
        ],
        copts = copts,
        size = "small",
        includes = ["tests/fixtures/corpus"],
        deps = [ife, "@rules_cc//cc/runfiles"],
        # EVERY fixture in the manifest, not a subset: this test iterates the
        # whole manifest, so one missing from runfiles is a failure it reports
        # rather than a fixture it skips. Adding a fixture means adding it
        # here as well as to the manifest.
        data = [
            "//tests/corpus:v1_0_witness.test_slide",
            "//tests/corpus:v1_1_witness.test_slide",
            "//tests/corpus:cipher_iris.test_slide",
            "//tests/corpus:v1_tile_offsets_full_width.bin",
        ],
        # Any fixture's runfiles path resolves to the directory holding them
        # all; pass the 1.0 witness, as the other tests do.
        local_defines = ["IFE_BAZEL_RUNFILES"],
        args = ["_main/tests/corpus/v1_0_witness.test_slide"],
    )

    native.genrule(
        name = "future_spec",
        srcs = [
            "tests/fixtures/build_future_spec.py",
            "spec/ife_fields.json",
            "spec/ife_constants.json",
            "spec/ife_header.json",
        ],
        outs = [
            "tests/fixtures/future_versioning/ife_fields.json",
            "tests/fixtures/future_versioning/ife_constants.json",
            "tests/fixtures/future_versioning/ife_header.json",
            "tests/fixtures/future_versioning/ife_fixture_layout.hpp",
        ],
        cmd = "python3 $(location tests/fixtures/build_future_spec.py) " +
              "$$(dirname $(location spec/ife_header.json)) " +
              "$(RULEDIR)/tests/fixtures/future_versioning",
    )

    native.genrule(
        name = "future_generated",
        # generator/ must be declared: a genrule's sandbox contains exactly
        # its inputs, and `python3 -m generator` resolves the package from
        # the cwd.
        srcs = [":future_spec"] + native.glob(["generator/**/*.py"]),
        outs = [
            "tests/fixtures/future_versioning/generated/IFE_Blocks.hpp",
            "tests/fixtures/future_versioning/generated/IFE_Blocks.cpp",
            "tests/fixtures/future_versioning/generated/IFE_Validation.hpp",
            "tests/fixtures/future_versioning/generated/IFE_Validation.cpp",
        ],
        cmd = python_resolve +
              "$$PY -m generator " +
              "--schema-dir $(RULEDIR)/tests/fixtures/future_versioning " +
              "--out-dir $(RULEDIR)/tests/fixtures/future_versioning/generated " +
              "--docs-dir $(RULEDIR)/tests/fixtures/future_versioning/generated_docs",
    )

    # ── Tests ────────────────────────────────────────────────────────────
    # Include order mirrors CMake exactly, and matters: the PRODUCTION
    # generated dir precedes the FIXTURE generated dir, so "IFE_Blocks.hpp"
    # resolves to the 1.1 layer in the backward test while the forward test
    # sees only the 200.0 layer.

    # IFE_Blocks.hpp textually #includes its own .cpp under IFE_HEADER_ONLY,
    # so that .cpp must be an INPUT of the header-only test's compile action
    # without ever being compiled itself. A hdrs-only library (no srcs)
    # declares exactly that: files for dependents, nothing to build.
    cc_library(
        name = "ife_header_only_inputs",
        hdrs = [
            src_headers,
            generated_headers,
        ],
    )

    # Scalar load/store primitives: the only place byte order and the packed
    # widths appear. Header-only — deliberately NOT linked against :ife.
    cc_test(
        name = "ife_bytes_tests",
        srcs = [
            "tests/ife_bytes_tests.cpp",
            src_headers,
            generated_headers,
        ],
        copts = copts,
        size = "small",
        includes = ["include", "generated_source"],
        deps = ["@iris_headers//:iris_headers"],
    )

    cc_test(
        name = "ife_blocks_tests",
        srcs = ["tests/ife_blocks_tests.cpp"],
        copts = copts,
        size = "small",
        deps = [ife],
    )

    # The same test compiled without linking :ife. IFE_HEADER_ONLY makes
    # IFE_Blocks.hpp include IFE_Blocks.cpp at the bottom of itself and marks
    # the definitions inline, so the test gets the whole layer from an
    # include and needs none of the library's symbols. The other consumption
    # path -- header declares, .cpp compiled once -- is what ife_blocks_tests
    # above exercises, which is why both exist.
    cc_test(
        name = "ife_blocks_header_only_tests",
        srcs = ["tests/ife_blocks_tests.cpp"],
        copts = copts,
        size = "small",
        local_defines = ["IFE_HEADER_ONLY"],
        includes = ["include", "generated_source"],
        deps = [
            ":ife_header_only_inputs",
            "@iris_headers//:iris_headers",
        ],
    )

    # IFE_HEADER_ONLY across TWO translation units in one binary.
    #
    # The single-TU test above cannot catch the failure that matters: without
    # `inline` on the folded definitions one TU still links. Two do not. Also
    # covers the circular include from both ends -- the header folds in the
    # .cpp and the .cpp opens by including the header -- so a lost guard is a
    # compile error here rather than a surprise in a consumer's tree.
    cc_test(
        name = "ife_header_only_odr_tests",
        srcs = [
            "tests/ife_header_only_odr_tests.cpp",
            "tests/ife_header_only_odr_second.cpp",
        ],
        copts = copts,
        size = "small",
        # IFE_HEADER_ONLY is defined by the sources themselves, not here: the
        # circular-include cases they exercise are about what the *file*
        # does, so a reader of the test should not have to check the build
        # for the define.
        includes = ["include", "generated_source"],
        deps = [
            ":ife_header_only_inputs",
            "@iris_headers//:iris_headers",
        ],
    )

    cc_test(
        name = "ife_window_tests",
        srcs = ["tests/ife_window_tests.cpp"],
        copts = copts,
        size = "small",
        deps = [ife],
    )

    # The public API end to end, against the fetched snapshot.
    cc_test(
        name = "ife_runtime_tests",
        srcs = [
            "tests/ife_runtime_tests.cpp",
            "tests/ife_v1_fixture.hpp",
            "tests/ife_corpus_path.hpp",
        ],
        copts = copts,
        size = "small",
        deps = [ife, "@rules_cc//cc/runfiles"],
        data = ["//tests/corpus:v1_0_witness.test_slide"],
        # Bazel cannot pass a directory, and Windows tests have no runfiles
        # tree (manifest-only mode) — so pass the manifest-style path and
        # resolve it through the runfiles library in ife_corpus_dir().
        # "_main" is the main repo's runfiles directory name under bzlmod.
        local_defines = ["IFE_BAZEL_RUNFILES"],
        args = ["_main/tests/corpus/v1_0_witness.test_slide"],
    )

    # Lifetime (XP-4): the abstraction reads over a lens of the caller's
    # mapping, so a view into a live mapping is the design and a view into a
    # temporary is the bug. The tree is built, the construction stack
    # unwinds, and every string it keeps is read. Only meaningful under a
    # sanitizer -- run it with --config=asan, or via CI's sanitizer leg --
    # but it is a correctness test either way, because the value checks catch
    # a corrupted read on their own.
    cc_test(
        name = "ife_lifetime_tests",
        srcs = [
            "tests/ife_lifetime_tests.cpp",
            "tests/ife_v1_fixture.hpp",
            "tests/ife_corpus_path.hpp",
        ],
        copts = copts,
        size = "small",
        deps = [ife, "@rules_cc//cc/runfiles"],
        data = ["//tests/corpus:v1_0_witness.test_slide"],
        # As ife_runtime_tests: Bazel cannot pass a directory, so pass the
        # manifest-style path of one corpus file and resolve it through the
        # runfiles library in ife_corpus_dir().
        local_defines = ["IFE_BAZEL_RUNFILES"],
        args = ["_main/tests/corpus/v1_0_witness.test_slide"],
    )

    # The v1 oracle: bytes written by the SHIPPED encoder, read back through
    # the generated layer. Needs :ife's STORE_* to run, not merely its
    # headers.
    cc_test(
        name = "ife_v1_oracle_tests",
        srcs = [
            "tests/ife_v1_oracle_tests.cpp",
            "tests/ife_v1_fixture.cpp",
            "tests/ife_v1_fixture.hpp",
            "tests/ife_corpus_path.hpp",
        ],
        copts = copts,
        size = "small",
        deps = [ife, "@rules_cc//cc/runfiles"],
        data = [
            "//tests/corpus:v1_0_witness.test_slide",
            "//tests/corpus:v1_tile_offsets_full_width.bin",
        ],
        # Manifest-style path resolved via the runfiles library (see
        # ife_runtime_tests); "_main" is the main repo under bzlmod.
        local_defines = ["IFE_BAZEL_RUNFILES"],
        args = ["_main/tests/corpus/v1_0_witness.test_slide"],
    )

    # The 1.1 witness: every 1.1 field present-and-correct on the 1.1 witness
    # and absent on the 1.0 snapshot — the pair to the v1 oracle.
    cc_test(
        name = "ife_v11_witness_tests",
        srcs = [
            "tests/ife_v11_witness_tests.cpp",
            "tests/ife_v11_fixture.cpp",
            "tests/ife_v11_fixture.hpp",
            "tests/ife_corpus_path.hpp",
        ],
        copts = copts,
        size = "small",
        deps = [ife, "@rules_cc//cc/runfiles"],
        data = [
            "//tests/corpus:v1_1_witness.test_slide",
            "//tests/corpus:v1_0_witness.test_slide",
        ],
        local_defines = ["IFE_BAZEL_RUNFILES"],
        args = ["_main/tests/corpus/v1_1_witness.test_slide"],
    )

    # A whole slide larger than 4 GiB. 64-bit hosts only, same as CMake.
    cc_test(
        name = "ife_large_file_tests",
        srcs = [
            "tests/ife_large_file_tests.cpp",
            "tests/ife_corpus_path.hpp",
        ],
        copts = copts,
        size = "small",
        # 64-bit hosts only (a 32-bit address space cannot map 4 GiB) — the
        # Bazel form of CMake's CMAKE_SIZEOF_VOID_P gate. select() has no
        # pointer-size test, so allowlist the 64-bit @platforms//cpu values
        # and let everything else (all 32-bit cpus, unknown cpus) fail
        # closed.
        target_compatible_with = select({
            "@platforms//cpu:x86_64": [],
            "@platforms//cpu:aarch64": [],
            "@platforms//cpu:arm64": [],
            "@platforms//cpu:arm64e": [],
            "@platforms//cpu:mips64": [],
            "@platforms//cpu:ppc64le": [],
            "@platforms//cpu:riscv64": [],
            "@platforms//cpu:s390x": [],
            "@platforms//cpu:wasm64": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        deps = [ife, "@rules_cc//cc/runfiles"],
        data = ["//tests/corpus:v1_0_witness.test_slide"],
        # "-" → $TEST_TMPDIR (see main()); corpus manifest path resolved via
        # the runfiles library like the other corpus tests.
        local_defines = ["IFE_BAZEL_RUNFILES"],
        args = [
            "-",
            "_main/tests/corpus/v1_0_witness.test_slide",
        ],
    )

    cc_test(
        name = "ife_validation_tests",
        srcs = ["tests/ife_validation_tests.cpp"],
        copts = copts,
        size = "small",
        deps = [ife_validation],
    )

    # Forward direction: the 200.0 build reads 1.0 and 200.0 files. This
    # test's include path sees ONLY the fixture layer — the production
    # generated dir is absent on purpose.
    cc_test(
        name = "ife_version_gating_tests",
        srcs = [
            "tests/ife_version_gating_tests.cpp",
            src_headers,
            "tests/fixtures/future_versioning/ife_fixture_layout.hpp",
            "tests/fixtures/future_versioning/generated/IFE_Blocks.hpp",
            "tests/fixtures/future_versioning/generated/IFE_Blocks.cpp",
        ],
        copts = copts,
        size = "small",
        # CMake include order: include, fixture generated, fixture root.
        includes = [
            "include",
            "tests/fixtures/future_versioning/generated",
            "tests/fixtures/future_versioning",
        ],
        deps = ["@iris_headers//:iris_headers"],
    )

    # Backward direction: the production 1.1 build reads the same 200.0
    # file. Production generated dir FIRST, so IFE_Blocks.hpp is the 1.1
    # layer; the fixture layer stays reachable for the file it must still
    # read.
    cc_test(
        name = "ife_version_gating_backward_tests",
        srcs = [
            "tests/ife_version_gating_backward_tests.cpp",
            src_headers,
            generated_headers,
            "tests/fixtures/future_versioning/ife_fixture_layout.hpp",
            "tests/fixtures/future_versioning/generated/IFE_Blocks.hpp",
        ],
        copts = copts,
        size = "small",
        includes = [
            "include",
            "generated_source",
            "tests/fixtures/future_versioning/generated",
            "tests/fixtures/future_versioning",
        ],
        deps = ["@iris_headers//:iris_headers"],
    )
