#!/usr/bin/env python3
"""Compile every C++ example in the README, as published.

The README's examples drifted from the API once while every test stayed green:
three blocks called types and methods that no longer existed (FastFHIR's README
learned the same lesson first). This gate extracts each ```cpp block and
compiles it syntax-only against the real headers, so an example that stops
compiling fails the build, naming the block.

Every ```cpp block must be preceded by an annotation (an HTML comment, which
GitHub does not render):

    <!-- ife-compile: fragment -->   statements: wrapped in a function, with the
                                     placeholder names from readme_context.hpp
                                     in scope (`path`, `ptr`, `size`, ...)
    <!-- ife-compile: program -->    a complete translation unit, compiled as is
    <!-- ife-compile: skip -->       an illustration, not code (a struct sketch)

A block with no annotation is an error: whoever adds an example decides which
it is. `#include` lines inside a fragment are hoisted to file scope.

Usage:
    readme_compiles.py --readme README.md --cxx c++ --include DIR [--include DIR ...]
    readme_compiles.py ... --dump N      # print the translation unit for block N

Stdlib only, like the generator.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FENCE = re.compile(r"^```cpp[ \t]*\n(.*?)^```", re.M | re.S)
ANNOTATION = re.compile(r"<!--\s*ife-compile:\s*(fragment|program|skip)\s*-->\s*$")


def blocks(readme: str) -> list[tuple[int, str, str | None]]:
    """(line, code, mode) for every ```cpp block; mode is None when unannotated."""
    found = []
    for match in FENCE.finditer(readme):
        line = readme.count("\n", 0, match.start()) + 1
        before = readme[: match.start()].rstrip("\n").splitlines()
        previous = before[-1].strip() if before else ""
        annotated = ANNOTATION.match(previous)
        found.append((line, match.group(1), annotated.group(1) if annotated else None))
    return found


def translation_unit(code: str, mode: str, index: int) -> str:
    if mode == "program":
        return code
    includes = [l for l in code.splitlines() if l.lstrip().startswith("#include")]
    body = "\n".join(l for l in code.splitlines() if not l.lstrip().startswith("#include"))
    return "\n".join([
        '#include "readme_context.hpp"',
        *includes,
        "",
        # Fragments name their placeholders unqualified.
        "using namespace readme;",
        f"void readme_block_{index}() {{",
        body,
        "}",
        "",
    ])


def platform_flags(cxx: str) -> list[str]:
    """Flags the configured compiler needs that CMake would otherwise supply.

    A toolchain clang++ -- which is exactly what the Xcode generator puts in
    CMAKE_CXX_COMPILER (XcodeDefault.xctoolchain/usr/bin/clang++) -- does not
    locate the macOS SDK on its own, so a syntax-only compile cannot find
    <cstddef>.  The /usr/bin shims do, which is why this only bites the Xcode
    build.  Hand it the SDK explicitly; empty everywhere but macOS.
    """
    if sys.platform != "darwin":
        return []
    try:
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True,
                             text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return []
    return ["-isysroot", sdk] if sdk else []


def compile_command(cxx: str, includes: list[str], source: Path,
                    extra: list[str]) -> list[str]:
    # MSVC (`cl`) and clang-cl take /Zs for syntax-only; clang and gcc take
    # -fsyntax-only.  Match the driver name exactly: a bare `startswith("cl")`
    # also matches `clang`/`clang++`, which would hand clang an MSVC command
    # line -- invisible under a generator that names the driver `c++`, fatal
    # under one that names it `clang++` (the Xcode generator does).
    name = Path(cxx).name.lower()
    if name.endswith(".exe"):
        name = name[:-4]
    if name == "cl" or name.startswith("clang-cl"):
        return [cxx, "/nologo", "/std:c++20", "/EHsc", "/Zs",
                *[f"/I{d}" for d in includes], str(source)]
    return [cxx, "-std=c++20", "-fsyntax-only", *extra,
            *[f"-I{d}" for d in includes], str(source)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--readme", required=True)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--dump", type=int)
    args = parser.parse_args()

    readme = Path(args.readme).read_text(encoding="utf-8")
    found = blocks(readme)
    includes = [str(HERE), *args.include]

    if args.dump is not None:
        line, code, mode = found[args.dump]
        print(translation_unit(code, mode or "fragment", args.dump))
        return 0

    failures = 0
    compiled = skipped = 0
    extra = platform_flags(args.cxx)
    with tempfile.TemporaryDirectory() as scratch:
        for index, (line, code, mode) in enumerate(found):
            if mode is None:
                print(f"README.md:{line}: block {index} has no <!-- ife-compile: ... --> "
                      "annotation (fragment, program or skip)", file=sys.stderr)
                failures += 1
                continue
            if mode == "skip":
                skipped += 1
                continue
            source = Path(scratch) / f"readme_block_{index}.cpp"
            source.write_text(translation_unit(code, mode, index), encoding="utf-8")
            result = subprocess.run(compile_command(args.cxx, includes, source, extra),
                                    capture_output=True, text=True)
            if result.returncode != 0:
                print(f"README.md:{line}: block {index} ({mode}) does not compile "
                      f"(--dump {index} prints it):\n{result.stdout}{result.stderr}",
                      file=sys.stderr)
                failures += 1
            else:
                compiled += 1

    print(f"readme_compiles: {compiled} compiled, {skipped} skipped, {failures} failed "
          f"of {len(found)} C++ blocks")
    return 1 if failures or not found else 0


if __name__ == "__main__":
    sys.exit(main())
