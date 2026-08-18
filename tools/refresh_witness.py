#!/usr/bin/env python3
"""Refresh tests/wire/witness.json from the current spec — deliberately.

The witness is append-only evidence, not output (see tests/wire/README.md):
it exists so --validate can see the diff between revisions, and it is
refreshed only as part of a reviewed wire change — never by the generator
and never by habit.

Guard, after FastFHIR's wire_witness.py dump():
  * a refresh that would record a wire *break* (a shipped fact changed or
    disappeared) is refused outright — there is no force flag, because the
    evidence must not bless a break;
  * a refresh that records additions prints the diff, so growing the
    evidence is a visible act, not a silent overwrite.

Usage: python3 tools/refresh_witness.py [--schema-dir spec]
"""

from __future__ import annotations

import argparse
import difflib
import json
import sys
from pathlib import Path

# The generator is a package under the repo root; make it importable no
# matter which CWD this tool is run from.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from generator.validate import check_permanence  # noqa: E402
from generator.witness import witness  # noqa: E402

_SECTIONS = ("blocks", "entries", "enums", "constants")


def _canonical(data: dict) -> str:
    """The canonical rendering the committed file is written in."""
    return json.dumps(data, indent=2, sort_keys=True) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema-dir", default="spec")
    args = parser.parse_args(argv)

    schema_dir = Path(args.schema_dir)
    docs = {
        name: json.loads((schema_dir / f"ife_{name}.json").read_text())
        for name in ("fields", "constants", "header")
    }
    fresh = witness(docs["fields"], docs["constants"], docs["header"])

    dest = schema_dir.parent / "tests" / "wire" / "witness.json"
    if dest.exists():
        existing = json.loads(dest.read_text())

        # 1. Never record a break: the gate must keep failing until the spec
        # is fixed, and a refreshed baseline would paper the break over.
        violations: list[str] = []
        for section in _SECTIONS:
            violations.extend(
                check_permanence(fresh[section], existing.get(section, {}), section)
            )
        if violations:
            for line in violations:
                print(line, file=sys.stderr)
            print(
                f"refresh refused: {len(violations)} wire fact(s) changed or "
                "disappeared — the witness must not record a break",
                file=sys.stderr,
            )
            return 1

        if _canonical(fresh) == _canonical(existing):
            print(f"wire witness unchanged — {dest} already matches")
            return 0

        # 2. Show what is being recorded (the additions), then write.
        print("recording:")
        for line in difflib.unified_diff(
            _canonical(existing).splitlines(),
            _canonical(fresh).splitlines(),
            fromfile=str(dest),
            tofile="fresh",
            lineterm="",
        ):
            print("  " + line)
    else:
        print(f"no baseline at {dest} — creating it")

    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(_canonical(fresh))
    print(f"wire witness written: {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
