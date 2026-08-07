"""IFE code generator — command-line entry point.

Phase 3 deliverable (MIGRATION.md). Emits the C++ serialization layer
(vtables, enums, sentinels) and the derived doc tables from the committed
spec JSON in `spec/`, into `generated_source/` and `generated_docs/`.

The CLI contract here is final: CMake's configure-time generation
(IFE_RUN_GENERATOR, or auto when generated_source/ lacks its anchor file)
invokes exactly this command line. Do not change flags without updating
CMakeLists.txt and generator/README.md in the same change.
"""
import argparse
import sys
from pathlib import Path

from . import pipeline


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m generator",
        description="Generate the IFE C++ serialization layer and derived layout tables.",
    )
    parser.add_argument(
        "--schema-dir",
        default="spec",
        help="Directory holding ife_fields.json and ife_constants.json (default: spec/)",
    )
    parser.add_argument(
        "--out-dir",
        default="generated_source",
        help="Directory to write generated C++ into (default: generated_source/)",
    )
    parser.add_argument(
        "--docs-dir",
        default="generated_docs",
        help="Directory to write generated documentation into (default: generated_docs/)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Verify outputs match a fresh regeneration (exit 1 on drift)",
    )
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    return pipeline.run(
        schema_dir=Path(args.schema_dir),
        out_dir=Path(args.out_dir),
        docs_dir=Path(args.docs_dir),
        check=args.check,
    )


if __name__ == "__main__":
    sys.exit(main())
