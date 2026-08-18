#!/usr/bin/env python3
"""Catch, in a second, the portability failures that otherwise take a CI round-trip.

Windows containers cannot run on a Linux or macOS host — a container shares
the host kernel rather than virtualising one — so the MSVC job is the first
place these are ever seen. Every failure this project has had there was
visible in the source:

    PLANES                     a <wingdi.h> macro used as a field name
    FILE_END                   a <winbase.h> macro used as a constant name
    IFE_V1_SLIDE_WRITER=...    a Windows path routed through a C string
                               literal, where its backslashes are escapes
    ctest with no -C           Visual Studio is a multi-config generator and
                               finds no executables without one
    __out                      a <sal.h> SAL annotation macro, defined by
                               MSVC's <yvals.h> in every translation unit,
                               used as a parameter name
    IFE_Window.hpp             a public header Bazel's sandbox could not see
                               because src_headers never declared it
    emcc/g++ -I src            hand-compiled jobs still using the pre-move
                               include path after the headers went to include/

so this checks for them directly. It is not a substitute for the MSVC or
Bazel jobs; it is the part of them that does not need those hosts.

Usage: tools/portability_lint.py [--quiet]     (non-zero exit on any finding)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from generator.validate import _PLATFORM_MACROS  # noqa: E402

SOURCE_DIRS = ("src", "include", "tests", "examples")
SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".cc")


def _strip_comments_and_strings(text: str) -> str:
    """Blank out comments and string literals, preserving line structure.

    A macro name inside a comment or a diagnostic message is not a
    declaration, and flagging one trains people to ignore the linter.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        two = text[i : i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif text[i] in "\"'":
            quote, j = text[i], i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def check_platform_macros() -> list[str]:
    """Identifiers *declared* in C++ sources that a platform defines as macros.

    Declarations only. A use of a genuine Windows macro is legitimate; a
    declaration of something with the same spelling is what breaks, because
    the preprocessor rewrites it before any namespace applies.
    """
    found: list[str] = []
    names = "|".join(re.escape(k) for k in sorted(_PLATFORM_MACROS, key=len, reverse=True))
    # Declarations only. Requiring a preceding *type* is what separates
    # `constexpr Offset FILE_END = ...` from `if (p == NULL)` and
    # `f(x, NULL)`, both of which are ordinary uses of a real macro and must
    # not be reported -- a linter that cries wolf is one people learn to skip.
    patterns = (
        # Type NAME = / ; / [ / (
        re.compile(rf"\b[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*\s*[*&]?\s+({names})\b\s*(?==[^=]|;|\[|\()"),
        # enum member or brace-init at the start of a line
        re.compile(rf"^\s*({names})\b\s*(?:=[^=]|,|$)"),
        # #define NAME
        re.compile(rf"^\s*#\s*define\s+({names})\b"),
    )
    for directory in SOURCE_DIRS:
        base = ROOT / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            code = _strip_comments_and_strings(path.read_text(encoding="utf-8", errors="replace"))
            for number, line in enumerate(code.splitlines(), 1):
                hits = {m.group(1) for p in patterns for m in p.finditer(line)}
                for name in sorted(hits):
                    found.append(
                        f"{path.relative_to(ROOT)}:{number}: {name!r} is a macro in "
                        f"{_PLATFORM_MACROS[name]}. The preprocessor rewrites it before "
                        f"namespaces apply, so this breaks on MSVC only. Rename it."
                    )
    return found


# SAL annotations, from <sal.h>. Kept apart from _PLATFORM_MACROS because they
# differ in both ways that change the check: MSVC's <yvals.h> includes <sal.h>
# in *every* translation unit, so the collision needs no windows.h and no
# #include at all; and the annotations expand to nothing, so a *use* breaks as
# surely as a declaration — `__out.clear()` reaches the compiler as `.clear()`.
# Any occurrence is therefore a finding. The list is the stable core of
# sal.h's old-style annotations; the _Null_-family and __drv_* sets are
# version-dependent and deliberately absent.
_SAL_ANNOTATIONS: dict[str, str] = {
    "__in": "<sal.h> parameter annotation",
    "__out": "<sal.h> parameter annotation",
    "__inout": "<sal.h> parameter annotation",
    "__in_opt": "<sal.h> parameter annotation",
    "__out_opt": "<sal.h> parameter annotation",
    "__inout_opt": "<sal.h> parameter annotation",
    "__deref": "<sal.h> dereference annotation",
    "__deref_opt": "<sal.h> dereference annotation",
    "__deref_in": "<sal.h> dereference annotation",
    "__deref_out": "<sal.h> dereference annotation",
    "__deref_inout": "<sal.h> dereference annotation",
    "__deref_opt_in": "<sal.h> dereference annotation",
    "__deref_opt_out": "<sal.h> dereference annotation",
    "__deref_opt_inout": "<sal.h> dereference annotation",
    "__ecount": "<sal.h> element-count annotation",
    "__bcount": "<sal.h> byte-count annotation",
    "__xcount": "<sal.h> element-count annotation",
    "__field": "<sal.h> field annotation",
    "__field_ecount": "<sal.h> field annotation",
    "__field_bcount": "<sal.h> field annotation",
    "__notnull": "<sal.h> null annotation",
    "__null": "<sal.h> null annotation",
    "__maybenull": "<sal.h> null annotation",
    "__success": "<sal.h> function annotation",
    "__checkReturn": "<sal.h> function annotation",
}


def check_sal_annotations() -> list[str]:
    """Identifiers that <sal.h> defines as macros in every MSVC translation unit.

    <yvals.h> — the base of the MSVC standard library — includes <sal.h>, so
    the annotations below are macros in every file MSVC compiles, not just
    files that include a Windows header. And they expand to nothing, so a use
    is as broken as a declaration. This is what the C2059 'syntax error: .'
    on IFE_Runtime.cpp was: `__out.clear()` parsed as `.clear()`.
    """
    found: list[str] = []
    names = "|".join(re.escape(k) for k in sorted(_SAL_ANNOTATIONS, key=len, reverse=True))
    pattern = re.compile(rf"\b({names})\b")
    # generated_source is scanned too: the emitter writes the same identifiers
    # the hand-written layer does, and the generated layer is compiled by MSVC.
    for directory in SOURCE_DIRS + ("generated_source",):
        base = ROOT / directory
        if not base.is_dir():
            # An absent generated_source is not a clean one. It is gitignored
            # and written at configure time, so on a fresh clone this check
            # would otherwise report the generated layer clean without having
            # read a byte of it -- and the emitter writes the same identifiers
            # the hand-written layer does, which is the whole reason it is
            # scanned. Say so instead of passing.
            if directory == "generated_source":
                found.append(
                    "generated_source/ does not exist, so the generated layer was not "
                    "scanned for SAL collisions. Configure the project (or run "
                    "`python3 -m generator`) and re-run this lint."
                )
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            code = _strip_comments_and_strings(path.read_text(encoding="utf-8", errors="replace"))
            for number, line in enumerate(code.splitlines(), 1):
                for match in pattern.finditer(line):
                    name = match.group(1)
                    found.append(
                        f"{path.relative_to(ROOT)}:{number}: {name!r} is "
                        f"{_SAL_ANNOTATIONS[name]}, defined by MSVC's <yvals.h> in every "
                        "translation unit. Any use — declaration or call — expands to "
                        "nothing on MSVC. Rename it."
                    )
    return found


def check_paths_through_string_literals() -> list[str]:
    """CMake definitions that put a filesystem path into a C string literal.

    `FOO="$<TARGET_FILE:bar>"` becomes `"D:\\a\\proj\\bar.exe"` in the source,
    and the compiler reads `\\a` as a bell and `\\b` as a backspace. The value
    is silently wrong on Windows and correct everywhere else. Pass the path as
    a test argument or an environment variable instead.
    """
    found: list[str] = []
    pattern = re.compile(r'^\s*([A-Za-z_][\w]*)\s*=\s*"\$<TARGET_(?:FILE|PROPERTY)')
    for path in sorted(ROOT.rglob("CMakeLists.txt")) + sorted(ROOT.rglob("*.cmake")):
        if not path.is_file():
            continue  # an Xcode build dir contains a directory literally named `.cmake`
        # Skip the fetched dependency tree and every build directory — plain
        # `build` and the -suffixed variants (build-xcode, build-asan) alike.
        # These are gitignored; nothing committed can be inside them.
        if any(part == ".deps" or part.startswith("build") for part in path.parts):
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = pattern.match(line)
            if match:
                found.append(
                    f"{path.relative_to(ROOT)}:{number}: {match.group(1)} carries a path "
                    "into a C string literal; a Windows path's backslashes become escape "
                    "sequences there. Pass it as a test argument instead."
                )
    return found


def check_ctest_configuration() -> list[str]:
    """ctest without -C in a workflow job that includes a Windows runner.

    Visual Studio and Ninja Multi-Config choose a configuration when they
    build and again when they test. Without one, every test reports "Test not
    available without configuration" and the job fails having run nothing.
    """
    found: list[str] = []
    workflows = ROOT / ".github" / "workflows"
    if not workflows.is_dir():
        return found
    for path in sorted(workflows.glob("*.y*ml")):
        text = path.read_text(encoding="utf-8")
        # Job boundaries are two-space-indented keys; good enough to tell
        # which job a `ctest` line belongs to without a YAML round-trip.
        jobs = re.split(r"^  (?=\w[\w-]*:)", text, flags=re.M)
        for job in jobs:
            if "windows" not in job:
                continue
            for number, line in enumerate(job.splitlines(), 1):
                if re.search(r"\bctest\b", line) and not re.search(r"(-C|--build-config)\s", line):
                    name = job.split(":", 1)[0].strip() or path.stem
                    found.append(
                        f"{path.relative_to(ROOT)}: job {name!r} runs ctest without -C "
                        "but its matrix includes a Windows runner, where the "
                        "configuration is chosen at test time: "
                        f"{line.strip()}"
                    )
    return found


def check_bazel_declared_headers() -> list[str]:
    """Public headers Bazel's sandbox cannot see.

    Bazel compiles in a sandbox where only declared inputs exist, so a header
    that is not in the filegroup a rule takes as `hdrs` is 'No such file or
    directory' on every host — Windows spells the same failure 'undeclared
    inclusion(s)'. The `src_headers` filegroup exists to declare the public
    headers, so every header in include/ must be listed there; a header moved
    into include/ without that edit costs the same round-trip the macros
    above do.
    """
    found: list[str] = []
    build = ROOT / "BUILD.bazel"
    if not build.is_file():
        return found
    text = build.read_text(encoding="utf-8")
    match = re.search(r'name = "src_headers",\s*srcs = \[(.*?)\]', text, re.S)
    if not match:
        return found
    declared = set(re.findall(r'"([^"]+)"', match.group(1)))
    for path in sorted((ROOT / "include").glob("*.hpp")):
        rel = f"include/{path.name}"
        if rel not in declared:
            found.append(
                f"BUILD.bazel: src_headers does not declare {rel}; the sandbox hides "
                "undeclared headers, so any TU including it fails to compile."
            )
    return found


def check_workflow_include_paths() -> list[str]:
    """Hand-compiled workflow jobs that still use the pre-move include path.

    The public headers live in include/. A job that compiles a src/ or tests/
    file with explicit -I flags and omits -I include is looking at the old
    layout and fails exactly like the Bazel sandbox does. The tell is `-I src`
    — the pre-move path — on the compile line or on the INC-style assignment
    it references.
    """
    found: list[str] = []
    workflows = ROOT / ".github" / "workflows"
    if not workflows.is_dir():
        return found
    compiles_source = re.compile(r"(?:^|[\s/])(?:src|tests)/[\w/]+\.(?:cpp|cc)\b")
    compiler = re.compile(r"\b(?:emcc|g\+\+|clang\+\+|c\+\+)\b")
    for path in sorted(workflows.glob("*.y*ml")):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            # YAML comments describe; they do not compile.
            if line.lstrip().startswith("#"):
                continue
            # A line that gets its include path from a shell variable is not
            # inspectable here, and the assignment it came from is checked on
            # its own line. Flagging it anyway is how a linter earns --quiet.
            if re.search(r"\$\{?\w+\}?", line.replace("${{", " ")):
                interpolated = True
            else:
                interpolated = False
            old_layout = bool(re.search(r"-I ?\bsrc\b", line)) and "-I include" not in line
            hand_compile = bool(compiler.search(line) and compiles_source.search(line)
                                and "-I" in line and not interpolated)
            if old_layout or (hand_compile and "-I include" not in line):
                found.append(
                    f"{path.relative_to(ROOT)}:{number}: compiles repository sources "
                    "without -I include, after the public headers moved to include/: "
                    f"{line.strip()}"
                )
    return found


CHECKS = (
    ("platform macros in declarations", check_platform_macros),
    ("SAL annotations in identifiers", check_sal_annotations),
    ("paths through string literals", check_paths_through_string_literals),
    ("ctest without a configuration", check_ctest_configuration),
    ("Bazel-declared headers", check_bazel_declared_headers),
    ("workflow include paths", check_workflow_include_paths),
)


def main() -> int:
    quiet = "--quiet" in sys.argv
    total = 0
    for label, check in CHECKS:
        findings = check()
        total += len(findings)
        if findings:
            print(f"portability: {label}", file=sys.stderr)
            for finding in findings:
                print(f"  {finding}", file=sys.stderr)
        elif not quiet:
            print(f"portability: {label} — clean")
    if total:
        print(f"portability: {total} finding(s)", file=sys.stderr)
        return 1
    if not quiet:
        print("portability: nothing to report")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
