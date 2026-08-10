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

# The complementary failure, and the quieter one. `include::` is a directive
# only at the start of a line; anywhere else Asciidoctor treats it as ordinary
# text and prints it. There is no warning, the exit code is 0, and the check
# above stays silent because nothing was left unresolved -- it was never
# resolved at all. Six value tables shipped this way before anyone noticed.
if grep -q 'include::' "$out/ife_spec.html"; then
    grep -o 'include::[^<)]*' "$out/ife_spec.html" | sort -u >&2
    echo "error: an include directive was printed instead of processed." >&2
    echo "       include:: must begin at column 0. To attach a table to a list" >&2
    echo "       item, end the item, put a '+' on its own line, then the" >&2
    echo "       directive on the line after it." >&2
    exit 1
fi

# And the third: a table that is generated but that nothing includes is a table
# no reader ever sees. Cheap to check, and it is how a new enumeration goes
# missing -- the generator writes it, --check is happy, and the document simply
# does not mention it.
missing=""
for table in "$root"/generated_docs/constants/*.adoc "$root"/generated_docs/layout/*.adoc; do
    [ -e "$table" ] || continue
    name="${table#"$root"/generated_docs/}"
    grep -qF "$name" "$root/spec/ife_spec.adoc" || missing="$missing $name"
done
if [ -n "$missing" ]; then
    echo "error: generated tables that the narrative never includes:" >&2
    for m in $missing; do echo "         $m" >&2; done
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
