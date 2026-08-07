# `generator/` — the IFE code generator

Phase 3 deliverable (see `MIGRATION.md`). Turns the committed spec JSON
(`spec/ife_fields.json`, `spec/ife_constants.json`) into the C++ that
Iris-File-Extension compiles — and the derived layout tables the
LaTeX/HTML specification document renders.

Run it with:

```bash
python3 -m generator                    # writes generated_source/ + generated_docs/
python3 -m generator --out-dir DIR      # write the C++ elsewhere
python3 -m generator --check            # CI: exit 1 if outputs drifted
```

## Output contract

| Output | Contract |
|--------|----------|
| `spec/*.json` | **Source of truth. Committed.** Fields, types, values. |
| `generated_source/` | **Freely regenerable. Gitignored.** C++ layer: `IFE_Constants.hpp`, `IFE_VTables.hpp`. Rebuilt whenever missing at CMake configure, or on demand with `-DIFE_RUN_GENERATOR=ON`. |
| `generated_docs/` | **Freely regenerable. Gitignored.** Doc tables: `layout_tables.md`. |

Nothing generated is permanent — but the *vtable layout* is, which is why
the JSON is versioned append-only.

## CLI contract (final)

| Flag | Default | Meaning |
|------|---------|---------|
| `--schema-dir` | `spec/` | Directory holding `ife_fields.json` + `ife_constants.json` |
| `--out-dir` | `generated_source/` | Where generated C++ is written |
| `--docs-dir` | `generated_docs/` | Where generated documentation is written |
| `--check` | — | Regenerate in memory and fail (exit 1) if outputs drifted |

CMakeLists.txt invokes exactly this interface at configure time; changing
flags requires updating CMakeLists.txt in the same change.

## Reading this as a C++ developer

Python's looseness hides structure that C++ states outright. This is the
translation table for the idioms this codebase actually uses — nothing else.

| Python here | C++ you already know | Caveat that matters |
|---|---|---|
| a `.py` file | a translation unit | No header. The file *is* the interface; there are no forward declarations. |
| `from .model.layout import derive_layout` | `#include` + `using` | Imports a **name**, not a file's text. Circular imports fail loudly rather than needing guards. |
| `_leading_underscore` | `static` / file-local | **Convention only.** Nothing enforces it — the compiler will not stop you. |
| `@dataclass(frozen=True)` | `struct` with `const` members | Generates the constructor and `operator==`. `frozen=True` makes assignment a runtime error, not a compile error. |
| `dict[str, X]` | `std::map<std::string, X>` | **Iterates in insertion order, guaranteed.** This is load-bearing here — see below. |
| `list[X]` | `std::vector<X>` | Grows freely; no reserve, no capacity. |
| `tuple[X, ...]` | `const std::vector<X>` | Immutable. Used for dataclass fields so a layout cannot be edited after derivation. |
| `X \| None` | `std::optional<X>` / nullable pointer | `None` is a value, not a null pointer. Test with `is None`. |
| `def f() -> Iterator[X]` + `yield` | a lazy range / coroutine | Produces values one at a time; nothing is computed until iterated. |
| `[f(x) for x in xs if p(x)]` | `std::transform` + `copy_if` | One expression, evaluated eagerly into a `list`. |
| `f"{name} = {value}"` | `std::format` | Interpolates any expression inside the braces. |
| `raise SpecError(...)` | `throw` | Unwinds the same way. `SpecError` derives from `ValueError`. |
| `if not fields:` | `if (fields.empty())` | Empty containers, `0`, `""` and `None` are all false. A frequent source of surprise. |
| `x: int` annotations | declarations | **Not enforced at runtime.** They document intent and drive editors; Python will happily pass a string. |

**Two properties this generator depends on, which have no C++ equivalent:**

1. **`dict` preserves insertion order.** Field order in the JSON *is* the byte
   order on the wire, and recovery-tag order *is* the sequence. Reordering a
   JSON object is therefore a wire change, not cosmetics. `--validate` guards
   the tag case explicitly because nothing in the language does.
2. **Strings are built, not streamed.** Every emitter appends lines to a
   `list[str]` and finishes with `"\n".join(out)` — the equivalent of filling a
   `std::vector<std::string>` and joining once, rather than writing to a
   stream. It is what makes output byte-stable and easy to diff.

**The data flow, end to end:**

```
spec/*.json  ->  model/layout.py   derive offsets and sizes (no I/O, no strings)
             ->  emit/cpp.py       render C++ text from that layout
             ->  emit/docs.py      render Markdown tables from the same layout
             ->  validate.py       conflict and reference checks over the JSON
             ->  pipeline.py       orchestrate: load, validate, render, write/compare
             ->  __main__.py       argument parsing only
```

`model/` computes, `emit/` formats, `validate/` checks, `pipeline` decides what
to do with the result. No module reaches across those lines: the model never
produces text, and the emitters never do arithmetic on offsets.

## Module map

| Module | Role |
|--------|------|
| `__main__.py` | CLI entry point (`python -m generator`); contract above. |
| `pipeline.py` | Stage orchestration: load JSON → derive → emit → `--check`. No code generation lives here. |
| `model/layout.py` | Layout derivation — the single implementation of the offset/size rules; offsets are never read from the JSON. |
| `emit/cpp.py` | C++ emission: `IFE_Constants.hpp` (enums + sentinels), `IFE_VTables.hpp` (vtables). |
| `emit/docs.py` | Markdown emission: derived layout tables (first cut of the doc emitter; full spec assembly is Phase 5). |

CMake's configure-time gate (`EXISTS generator/pipeline.py`) runs
`python -m generator` whenever `generated_source/IFE_VTables.hpp` is
missing or `-DIFE_RUN_GENERATOR=ON`.

## Contract

* Output must be byte-stable (stable ordering, no timestamps) so CI can
  diff-check regeneration with `--check`.
* Never hand-edit `generated_source/` or `generated_docs/`; fix the JSON
  and regenerate.
