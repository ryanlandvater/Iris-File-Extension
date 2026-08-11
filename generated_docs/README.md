# `generated_docs/` — the generated half of the specification

Freely regenerable AsciiDoc produced by `generator/` from the committed spec
JSON. Gitignored like `generated_source/` — the JSON is the source of truth,
and nothing here is hand-edited. `README.md` is the only committed file.

| Path | Content |
|------|---------|
| `layout/<BLOCK>.adoc` | One block's header table: field, type, derived offset, size, optionality, and description. |
| `layout/<BLOCK>_entry.adoc` | That block's array-entry table, where it has one. |
| `layout/primitive_<NAME>.adoc` | A shared prefix every derived block carries. |
| `constants/<GROUP>.adoc` | One enumeration, or the statically defined values. |
| `provenance.adoc` | Specification version, status, and the generator version that rendered the document. |

## Why one file per item

`spec/ife_spec.adoc` pulls each in with Asciidoctor's native `include::`. Fine
grained, as the Vulkan specification's are: the narrative includes exactly what
it needs where it needs it, and moving a section never drags an unrelated table
along. There is no preprocessor and no bespoke anchor syntax anywhere in the
pipeline — that is the whole reason the source is AsciiDoc rather than Markdown.

## Build the document

```bash
spec/build_document.sh            # -> build/doc/ife_spec.{html,pdf}
```

Needs `asciidoctor` (HTML) and `asciidoctor-pdf` (PDF, Ruby >= 3.2):

```bash
gem install asciidoctor asciidoctor-pdf
```

## Contract

* Never hand-edit anything here; fix the JSON and regenerate.
* Output is byte-stable, so `python3 -m generator --check` covers these files
  exactly as it covers the C++.
* A file the current spec no longer produces is an **orphan**: `--check` fails
  on it and regeneration removes it. Without that, renaming a block would leave
  its old table on disk and the narrative would keep including it — publishing
  content with no source, silently.
