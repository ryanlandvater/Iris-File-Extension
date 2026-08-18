# tests/wire — the append-only witness

`witness.json` is the wire witness: a snapshot of every fact the IFE
specification documents that actually reaches the wire — block recovery tag
values, primitive inheritance, every field's (name, type, width, offset) per
version group, array entry sizes per version group, enum members and
statically defined values — derived from `spec/*.json` through
`generator.model.layout.derive_layout`, the same derivation the emitters and
the reader use. `python3 -m generator --validate` recomputes it and fails if
anything a shipped revision recorded has changed or disappeared; additions
pass silently, because that is what append-only means.

This is **append-only evidence, not a snapshot to refresh casually.** A
shipped field that moves, widens, or retypes is invisible to every
within-document check in `generator/validate.py`; this file exists to make
that diff visible at `--validate` time. Refresh it deliberately — and only
after a reviewed wire change — with:

```bash
python3 - <<'EOF'
import json
from pathlib import Path
import generator.witness as w
docs = {name: json.loads(Path(f"spec/ife_{name}.json").read_text())
        for name in ("fields", "constants", "header")}
Path("tests/wire/witness.json").write_text(
    json.dumps(w.witness(docs["fields"], docs["constants"], docs["header"]),
               indent=2, sort_keys=True) + "\n")
EOF
```

One caveat: this baseline records today's tree, which already contains the
deliberate 1.0 `ATTRIBUTE_SIZE` correction — KIND added, entry six to seven
bytes, commit `9e591fa5e9e71274cde3de5415fa55df6c82f033` — so the gate starts
from a state whose history includes the one break of the append-only
invariant, and must not be read as proof it has held unbroken.
