# `generated_docs/` — generated specification tables

Freely regenerable Markdown/LaTeX/HTML output produced by `generator/`
(the doc emitter) from the committed spec JSON. Gitignored like
`generated_source/` — the JSON is the source of truth, and nothing here
is hand-edited.

| File | Content |
|------|---------|
| `layout_tables.md` | Derived per-block/entry layout tables — the content of the `{{layout:BLOCK}}` / `{{entry_layout:BLOCK}}` anchors. |

The document pipeline will add enumeration tables (`{{constants:group}}`)
and assemble the full LaTeX/HTML document from the `spec/ife_spec.md`
basis. `README.md` is the only committed file here.
