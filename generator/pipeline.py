"""Pipeline orchestration for the IFE code generator.

Loads the committed spec JSON, derives all layouts (generator/model),
emits the C++ layer and doc tables (generator/emit), and supports
--check for CI drift detection. No code-generation logic lives here.
"""
# ---------------------------------------------------------------------------
# ROLE: orchestration and the only file I/O in the generator. Loads the three
# spec documents, validates them, asks model/ for a layout, asks emit/ for
# text, and then either writes the result or compares it (--check).
#
# `outputs` maps relative path -> full file contents, built entirely in memory
# before anything is written. Adding a generated file means adding one entry
# to that map: _write_if_changed, _run_check and the drift gate are already
# generic over it and need no change.
# ---------------------------------------------------------------------------

from __future__ import annotations

import json
import sys
from pathlib import Path

from .emit.cpp import (
    emit_validation_header,
    emit_validation_source,
    emit_blocks_header,
)
from .emit.docs import emit_documents
from .model.layout import RECOVERY_PREFIX, LayoutResult, derive_layout
from .validate import validate

# The generator is part of the standard once it renders the document, so the
# published artifact states which version produced it.
GENERATOR_VERSION = "1.0"

_CPP_ROOT = "generated_source"
_DOCS_ROOT = "generated_docs"


def _render(
    fields_doc: dict, constants_doc: dict, header: dict
) -> tuple[dict[str, str], LayoutResult]:
    """Render every output: relative path -> content (byte-stable)."""
    layout = derive_layout(fields_doc, constants_doc)
    outputs = {
        f"{_CPP_ROOT}/IFE_Blocks.hpp": emit_blocks_header(
            layout, fields_doc.get("types", {}), constants_doc, header
        ),
        f"{_CPP_ROOT}/IFE_Validation.hpp": emit_validation_header(layout, header),
        f"{_CPP_ROOT}/IFE_Validation.cpp": emit_validation_source(
            layout, constants_doc, fields_doc.get("types", {}), header
        ),
    }
    # The specification document's generated half: one AsciiDoc file per item,
    # so the narrative includes exactly what it needs where it needs it.
    for rel, text in emit_documents(
        layout, constants_doc, header, GENERATOR_VERSION, RECOVERY_PREFIX
    ).items():
        outputs[f"{_DOCS_ROOT}/{rel}"] = text
    return outputs, layout


def _dest_dir(rel: str, out_dir: Path, docs_dir: Path) -> Path:
    return out_dir if rel.startswith(_CPP_ROOT) else docs_dir


def _relative(rel: str) -> str:
    """The path below the output root.

    Not just the basename: the document emitter writes into layout/ and
    constants/ subdirectories, and spec/ife_spec.adoc includes them by those
    names, so the structure is part of the contract rather than cosmetic.
    """
    return rel.split("/", 1)[1]


# Writes only when the content differs, so unchanged files keep their mtime and
# CMake does not rebuild the world after a no-op regeneration.
def _write_if_changed(path: Path, text: str) -> bool:
    """Write only when content differs so mtimes stay stable for CMake."""
    if path.exists() and path.read_text() == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return True


# Extensions the generator owns. Anything else under an output root -- a
# committed README, an editor's scratch file -- is left alone.
_OWNED = {".hpp", ".cpp", ".adoc", ".md"}


def _orphans(outputs: dict[str, str], out_dir: Path, docs_dir: Path) -> list[Path]:
    """Generated files on disk that this render no longer produces.

    A renamed or deleted block leaves its old artifact behind, and nothing
    else notices: the C++ GLOB would compile a stale header, and -- worse --
    the specification document would keep including a table for a block that
    no longer exists, publishing content with no source. Both are silent, so
    the pipeline that owns these directories has to own their removal too.
    """
    expected = {
        (_dest_dir(rel, out_dir, docs_dir) / _relative(rel)).resolve()
        for rel in outputs
    }
    found: list[Path] = []
    for root in {out_dir, docs_dir}:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in _OWNED:
                continue
            if path.name == "README.md":
                continue   # committed documentation, not generated output
            if path.resolve() not in expected:
                found.append(path)
    return sorted(found)


def _run_check(outputs: dict[str, str], out_dir: Path, docs_dir: Path) -> int:
    drifted: list[str] = []
    missing: list[str] = []
    for rel, text in outputs.items():
        path = _dest_dir(rel, out_dir, docs_dir) / _relative(rel)
        if not path.exists():
            missing.append(rel)
        elif path.read_text() != text:
            drifted.append(rel)
    for rel in missing:
        print(f"generator: missing {rel}", file=sys.stderr)
    for rel in drifted:
        print(f"generator: drifted {rel}", file=sys.stderr)
    orphans = _orphans(outputs, out_dir, docs_dir)
    for path in orphans:
        print(f"generator: orphaned {path}", file=sys.stderr)
    if missing or drifted or orphans:
        return 1
    print("generator: outputs are current (no drift)")
    return 0


def run(
    schema_dir: Path,
    out_dir: Path,
    docs_dir: Path,
    check: bool = False,
    validate_only: bool = False,
) -> int:
    fields_path = schema_dir / "ife_fields.json"
    constants_path = schema_dir / "ife_constants.json"
    header_path = schema_dir / "ife_header.json"
    try:
        fields_doc = json.loads(fields_path.read_text())
        constants_doc = json.loads(constants_path.read_text())
        header = json.loads(header_path.read_text())
    except (OSError, ValueError) as exc:
        print(f"generator: {exc}", file=sys.stderr)
        return 2

    # Consistency before derivation: a dangling reference or a tag conflict
    # produces a clearer message here than a SpecError from the middle of
    # layout computation, and some conflicts derivation never notices at all.
    # The narrative, when it is beside the schema: only --validate needs it, so
    # generating C++ from a checkout without it still works.
    narrative_path = schema_dir / "ife_spec.adoc"
    narrative = narrative_path.read_text() if narrative_path.exists() else None
    problems = validate(fields_doc, constants_doc, header, narrative)
    for problem in problems:
        print(f"generator: {problem}", file=sys.stderr)
    if problems:
        print(f"generator: {len(problems)} consistency problem(s)", file=sys.stderr)
        return 1
    if validate_only:
        print("generator: spec documents are consistent")
        return 0

    try:
        outputs, layout = _render(fields_doc, constants_doc, header)
    except (OSError, ValueError) as exc:
        print(f"generator: {exc}", file=sys.stderr)
        return 2

    if check:
        return _run_check(outputs, out_dir, docs_dir)

    written = 0
    for rel, text in outputs.items():
        dest = _dest_dir(rel, out_dir, docs_dir) / _relative(rel)
        if _write_if_changed(dest, text):
            written += 1
    removed = 0
    for path in _orphans(outputs, out_dir, docs_dir):
        path.unlink()
        removed += 1
    summary = f"generator: wrote {written} file(s) into {out_dir} and {docs_dir}"
    if removed:
        summary += f", removed {removed} orphan(s)"
    print(summary)

    for block in layout.blocks.values():
        size = f"header {block.header_size} B"
        if block.from_sof is not None:
            size += f" (from SOF {block.from_sof} B)"
        if block.entry_size is not None:
            size += f", entry {block.entry_size} B"
        print(f"  {block.name:28} {size}")
    return 0
