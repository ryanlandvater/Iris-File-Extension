"""The generator must produce byte-identical output on every run.

Without this, you cannot diff two generator runs and trust that a difference is
a real change. The reference is FastFHIR's test_determinism.py: run the
generator twice into separate trees and compare every emitted file.

The seeds are the point: dict and set iteration order can vary with
PYTHONHASHSEED, so the caller runs this under the ambient environment AND
PYTHONHASHSEED=0 and =1 (ctest for the default, CI for the seeds). The
generator subprocess inherits the environment, so the seeds reach it.

Stdlib-only, like the generator itself: this runs as a plain script (non-zero
exit on failure) and is also collectable by pytest should one ever join the
toolchain.
"""

from __future__ import annotations

import filecmp
import os
import subprocess
import sys
import tempfile
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]


def _generate(out_root: Path) -> subprocess.CompletedProcess:
    """One full generation into a fresh tree."""
    return subprocess.run(
        [
            sys.executable,
            "-m",
            "generator",
            "--schema-dir", str(_REPO_ROOT / "spec"),
            "--out-dir", str(out_root / "generated_source"),
            "--docs-dir", str(out_root / "generated_docs"),
        ],
        cwd=_REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=1800,
    )


def _differences(a: Path, b: Path) -> list[str]:
    """Recursively collect every path where the two trees disagree."""
    out: list[str] = []

    def walk(cmp: filecmp.dircmp, prefix: str = "") -> None:
        out.extend(f"{prefix}{n} (differs)" for n in cmp.diff_files)
        out.extend(f"{prefix}{n} (only in first)" for n in cmp.left_only)
        out.extend(f"{prefix}{n} (only in second)" for n in cmp.right_only)
        out.extend(f"{prefix}{n} (unreadable)" for n in cmp.funny_files)
        for name, sub in cmp.subdirs.items():
            walk(sub, f"{prefix}{name}/")

    walk(filecmp.dircmp(str(a), str(b)))
    return out


def test_two_runs_are_byte_identical() -> None:
    """Two runs of the same code into separate trees must not differ."""
    with tempfile.TemporaryDirectory() as tmp:
        a, b = Path(tmp) / "run_a", Path(tmp) / "run_b"
        for out in (a, b):
            proc = _generate(out)
            if proc.returncode != 0:
                raise AssertionError(
                    f"`python -m generator` exited {proc.returncode}\n"
                    f"stderr tail:\n{proc.stderr[-1500:]}"
                )
        diffs = _differences(a, b)
        assert not diffs, (
            f"{len(diffs)} paths differ between two runs of the same code -- the "
            "generator is non-deterministic. Look for iteration over an "
            f"unordered set or dict. First few: {diffs[:8]}"
        )


if __name__ == "__main__":
    test_two_runs_are_byte_identical()
    seed = os.environ.get("PYTHONHASHSEED", "unset")
    print(f"ife_generator_determinism: two runs byte-identical (PYTHONHASHSEED={seed})")
