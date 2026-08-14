"""Shared python-interpreter resolution for IFE genrules.

Bazel's strict action environment (Bazel 7+) gives genrules a minimal PATH
that excludes the host's python — observed on the Windows runner as
`SET PATH=C:/Program Files/Git/usr/bin;...` with no interpreter. CI therefore
passes `--action_env=PATH` (see .github/workflows/ci.yml) so genrules inherit
the runner PATH. This leaves one platform naming difference to cover: some
runners ship `python3`, some only `python`.
"""

PYTHON_RESOLVE = (
    "PY=$$(command -v python3 || command -v python); " +
    "[ -n \"$$PY\" ] || { echo 'genrule: no python on PATH' >&2; exit 1; }; "
)
