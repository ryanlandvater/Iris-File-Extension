#!/usr/bin/env bash
# Build the IFE specification document — HTML and PDF — from one source.
#
#     spec/ife_spec.adoc          hand-written narrative and normative prose
#   + generated_docs/**/*.adoc    every layout and value table, from spec/*.json
#   = build/doc/ife_spec.{html,pdf}
#
# There is no preprocessor here and no hand-written layout content: Asciidoctor
# resolves `include::` itself, which is why the source is AsciiDoc rather than
# Markdown with a bespoke anchor syntax.
#
# Usage: spec/build_document.sh [output-dir]        (default: build/doc)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/build/doc}"
mkdir -p "$out"

# The generated half must be current, or the document renders a stale layout.
# Cheap, deterministic, and the same command CI runs.
python3 -m generator --docs-dir "$root/generated_docs" >/dev/null

command -v asciidoctor >/dev/null || {
    echo "asciidoctor not found." >&2
    echo "  macOS: brew install asciidoctor          (bundles asciidoctor-pdf and a modern Ruby)" >&2
    echo "  other: gem install asciidoctor asciidoctor-pdf   (needs Ruby >= 3.2)" >&2
    exit 1
}

# Document status lives in the provenance table, from ife_header.json --
# not in a banner the reader has to scroll past on every build.
common=(-a "generated=$root/generated_docs" -r asciidoctor)

# Left-aligned body text: the default theme justifies, which spaces words
# badly around the long identifiers this document is made of.
pdf_opts=(-a "pdf-theme=$root/spec/ife-pdf-theme.yml")

echo "==> HTML"
asciidoctor "${common[@]}" -o "$out/ife_spec.html" "$root/spec/ife_spec.adoc"

# Asciidoctor reports a missing include *in the output* and still exits 0, so
# the exit code is not the gate -- a renamed block would otherwise publish a
# document with a hole in it. Checked here rather than only in CI so a local
# build fails the same way.
if grep -q 'Unresolved directive' "$out/ife_spec.html"; then
    grep -o 'Unresolved directive[^<]*' "$out/ife_spec.html" >&2
    echo "error: the document includes generated content that does not exist" >&2
    exit 1
fi

if command -v asciidoctor-pdf >/dev/null; then
    echo "==> PDF"
    asciidoctor-pdf "${common[@]}" "${pdf_opts[@]}" -o "$out/ife_spec.pdf" "$root/spec/ife_spec.adoc"
else
    # asciidoctor-pdf needs a newer Ruby than some systems ship. HTML is the
    # gate that must always run; the PDF is the published artifact.
    echo "==> PDF skipped: asciidoctor-pdf not installed (needs Ruby >= 3.2)" >&2
fi

echo "==> $out"
ls -la "$out"
