"""Shared python-interpreter resolution for IFE genrules.

`command -v python3` alone is not enough on GitHub Windows runners: python3 can
resolve to the Microsoft Store stub — a real file that exits 9009 with no
output. bash reports that as exit 1 and the CI log shows nothing, which is
exactly how the corpus genrule failed before this rule existed. Probe each
candidate with a real invocation; the stub fails the probe and the working
interpreter wins. Diagnostics are emitted only on the failure path, so a
successful build stays silent (the fetch/generator output is the only noise).
"""

PYTHON_RESOLVE = (
    "PY=$$(for c in python3 python; do " +
    "command -v $$c >/dev/null 2>&1 && $$c -c 'import sys' >/dev/null 2>&1 " +
    "&& { echo $$c; break; }; done); " +
    "[ -n \"$$PY\" ] || { echo 'genrule: no usable python on PATH " +
    "(python3 and python both failed the probe)' >&2; exit 1; }; "
)
