#!/usr/bin/env python3
"""Refuse a release the specification does not support.

A release ships two things that carry a version: archives of binaries whose
IFE_SCHEMA_VERSION_MAJOR/_MINOR constants are generated from
spec/ife_header.json, and a tag naming a version to whoever downloads them.
Nothing else connects the two, so a mistyped tag publishes 1.2-labelled
archives full of 1.1 constants. The fix for that is a retag, which is why
this runs before anything is compiled rather than after.

It also refuses to publish a draft. `python3 -m generator --validate` already
requires a ratified revision to carry a date; this is the complementary rule,
that a release requires a ratified revision at all. A published binary is an
authority claim about a byte format, and "draft" is precisely the statement
that the format is not settled — the two cannot ship together.

Usage:
    tools/release_guard.py                  # ratification status only
    tools/release_guard.py --tag v1.1       # the release gate
    tools/release_guard.py --tag v1.1 --github-output "$GITHUB_OUTPUT"
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "spec" / "ife_header.json"

# v1.1 names the specification revision; v1.1.3 names a library release built
# against it -- a packaging or defect fix that moves no byte on the wire.
# Both are legitimate, and both must agree with the spec in their first two
# components. The third belongs to this repository alone, which is why it is
# optional here and absent from the spec.
TAG = re.compile(r"^v(?P<major>\d+)\.(?P<minor>\d+)(?:\.(?P<patch>\d+))?$")


def problems(document: dict, tag: str | None) -> list[str]:
    """Every reason this document and tag cannot become a release.

    Ordered so the first line printed is the one to act on: the specification's
    own state before the tag, because a draft cannot be released under any tag.
    """
    version = document.get("version", {})
    label = f"{version.get('major', 0)}.{version.get('minor', 0)}"
    found: list[str] = []

    entry = next(
        (r for r in document.get("revisions", []) if r.get("version") == label), None
    )
    if entry is None:
        found.append(
            f"spec/ife_header.json declares version {label}, but its revision history "
            "has no entry for it"
        )
    elif entry.get("status") != "ratified":
        found.append(
            f"revision {label} is {entry.get('status', 'unstated')!r}, not 'ratified'; "
            "a release publishes the specification as authoritative, so ratify it first"
        )

    if tag is None:
        return found

    match = TAG.match(tag)
    if match is None:
        found.append(f"tag {tag!r} is not vMAJOR.MINOR or vMAJOR.MINOR.PATCH")
        return found

    named = f"{match['major']}.{match['minor']}"
    if named != label:
        found.append(
            f"tag {tag!r} names specification version {named}, but spec/ife_header.json "
            f"declares {label}; the archives would carry {label} constants under a "
            f"{named} filename"
        )
    return found


def notes(document: dict) -> str:
    """The release body, from the revision history that already states it.

    Release notes and a revision summary are the same paragraph written for
    two audiences, and hand-copying one into the other is how they start
    disagreeing. The document is the source; this is a rendering of it.
    """
    version = document.get("version", {})
    label = f"{version.get('major', 0)}.{version.get('minor', 0)}"
    entry = next(
        (r for r in document.get("revisions", []) if r.get("version") == label), {}
    )

    ratified = f"Ratified {entry['date']}." if entry.get("date") else ""
    authors = ", ".join(entry.get("authors", []))
    body = [
        f"## Iris File Extension {label}",
        "",
        " ".join(x for x in (ratified, authors) if x),
        "",
        entry.get("summary", ""),
    ]
    if errata := entry.get("errata"):
        body += ["", f"> **Erratum.** {errata}"]

    body += [
        "",
        "### What is here",
        "",
        "One archive per platform, each carrying the shared and static "
        "libraries, the public headers, and the CMake package configuration "
        "that `find_package(IrisFileExtension)` resolves. The specification "
        "itself is published alongside them as PDF and HTML.",
        "",
        "Archives are pinned by digest in `SHA256SUMS`, and verified with",
        "",
        "```",
        "sha256sum -c SHA256SUMS",
        "```",
    ]
    return "\n".join(body) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Gate a release on the specification.")
    parser.add_argument("--tag", help="the git tag being released, e.g. v1.1")
    parser.add_argument("--spec", type=Path, default=SPEC, help="path to ife_header.json")
    parser.add_argument(
        "--github-output",
        type=Path,
        help="append 'version=MAJOR.MINOR' for later workflow steps to read",
    )
    parser.add_argument(
        "--notes-file",
        type=Path,
        help="write the release body, rendered from the revision history",
    )
    args = parser.parse_args()

    with args.spec.open(encoding="utf-8") as handle:
        document = json.load(handle)

    version = document.get("version", {})
    label = f"{version.get('major', 0)}.{version.get('minor', 0)}"

    # Written before the verdict, on purpose: which version this document
    # describes is a fact of the document, true whether or not it may ship.
    # A dry run names its archives from this while the gate below is still
    # refusing to publish them.
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8") as handle:
            handle.write(f"version={label}\n")

    if found := problems(document, args.tag):
        for problem in found:
            print(f"release: {problem}", file=sys.stderr)
        return 1

    agreement = f" and tag {args.tag} agrees" if args.tag else ""
    print(f"release: specification {label} is ratified{agreement}")

    # After the verdict, unlike the version: a body describing a release that
    # cannot happen has no reader.
    if args.notes_file:
        args.notes_file.write_text(notes(document), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
