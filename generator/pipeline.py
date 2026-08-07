"""Pipeline orchestration for the IFE code generator.

Loads the committed spec JSON, derives all layouts (generator/model),
emits the C++ layer and doc tables (generator/emit), and supports
--check for CI drift detection. No code-generation logic lives here.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

from .emit.cpp import emit_constants_header, emit_vtables_header
from .emit.docs import emit_layout_markdown
from .model.layout import LayoutResult, derive_layout

_CPP_ROOT = "generated_source"
_DOCS_ROOT = "generated_docs"


def _render(fields_doc: dict, constants_doc: dict) -> tuple[dict[str, str], LayoutResult]:
    """Render every output: relative path -> content (byte-stable)."""
    layout = derive_layout(fields_doc, constants_doc)
    outputs = {
        f"{_CPP_ROOT}/IFE_Constants.hpp": emit_constants_header(constants_doc, fields_doc.get("types", {})),
        f"{_CPP_ROOT}/IFE_VTables.hpp": emit_vtables_header(layout),
        f"{_DOCS_ROOT}/layout_tables.md": emit_layout_markdown(layout),
    }
    return outputs, layout


def _dest_dir(rel: str, out_dir: Path, docs_dir: Path) -> Path:
    return out_dir if rel.startswith(_CPP_ROOT) else docs_dir


def _write_if_changed(path: Path, text: str) -> bool:
    """Write only when content differs so mtimes stay stable for CMake."""
    if path.exists() and path.read_text() == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return True


def _run_check(outputs: dict[str, str], out_dir: Path, docs_dir: Path) -> int:
    drifted: list[str] = []
    missing: list[str] = []
    for rel, text in outputs.items():
        path = _dest_dir(rel, out_dir, docs_dir) / rel.rsplit("/", 1)[1]
        if not path.exists():
            missing.append(rel)
        elif path.read_text() != text:
            drifted.append(rel)
    for rel in missing:
        print(f"generator: missing {rel}", file=sys.stderr)
    for rel in drifted:
        print(f"generator: drifted {rel}", file=sys.stderr)
    if missing or drifted:
        return 1
    print("generator: outputs are current (no drift)")
    return 0


def run(schema_dir: Path, out_dir: Path, docs_dir: Path, check: bool = False) -> int:
    fields_path = schema_dir / "ife_fields.json"
    constants_path = schema_dir / "ife_constants.json"
    try:
        fields_doc = json.loads(fields_path.read_text())
        constants_doc = json.loads(constants_path.read_text())
        outputs, layout = _render(fields_doc, constants_doc)
    except (OSError, ValueError) as exc:
        print(f"generator: {exc}", file=sys.stderr)
        return 2

    if check:
        return _run_check(outputs, out_dir, docs_dir)

    written = 0
    for rel, text in outputs.items():
        dest = _dest_dir(rel, out_dir, docs_dir) / rel.rsplit("/", 1)[1]
        if _write_if_changed(dest, text):
            written += 1
    print(f"generator: wrote {written} file(s) into {out_dir} and {docs_dir}")

    for block in layout.blocks.values():
        size = f"header {block.header_size} B"
        if block.from_sof is not None:
            size += f" (from SOF {block.from_sof} B)"
        if block.entry_size is not None:
            size += f", entry {block.entry_size} B"
        print(f"  {block.name:28} {size}")
    return 0
