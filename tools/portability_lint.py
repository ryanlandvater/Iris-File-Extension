#!/usr/bin/env python3
"""Catch, in a second, the MSVC failures that otherwise take a CI round-trip.

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

so this checks for them directly. It is not a substitute for the MSVC job; it
is the part of that job that does not need Windows.

Usage: tools/portability_lint.py [--quiet]     (non-zero exit on any finding)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from generator.validate import _PLATFORM_MACROS  # noqa: E402

SOURCE_DIRS = ("src", "tests", "examples")
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
        if ".deps" in path.parts or "build" in path.parts:
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


CHECKS = (
    ("platform macros in declarations", check_platform_macros),
    ("paths through string literals", check_paths_through_string_literals),
    ("ctest without a configuration", check_ctest_configuration),
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
