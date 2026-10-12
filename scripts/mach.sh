#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export MOZBUILD_STATE_PATH="$(cygpath -m "$project_root/.cache/mozbuild")"
# Read-mostly toolchains may be shared between checkouts; see Enter-DevEnvironment.ps1.
shared_cache="${FENN_SHARED_CACHE:-$project_root/.cache}"
export CARGO_HOME="$(cygpath -m "$shared_cache/cargo")"
export RUSTUP_HOME="$(cygpath -m "$shared_cache/rustup")"
export PIP_CACHE_DIR="$(cygpath -m "$shared_cache/pip")"
export SCCACHE_DIR="$(cygpath -m "$project_root/.cache/sccache")"
# FENN_MOZCONFIG selects another file in configs/, e.g. mozconfig.release.
mozconfig="$project_root/configs/${FENN_MOZCONFIG:-mozconfig}"
[ -f "$mozconfig" ] || { echo "mach.sh: no such mozconfig: $mozconfig" >&2; exit 1; }
export MOZCONFIG="$(cygpath -m "$mozconfig")"
export PATH="$(cygpath -u "$CARGO_HOME")/bin:$PATH"
# Always run mach with MozillaBuild's Python. mach's virtualenvs record the
# interpreter that built them and are rebuilt when another one runs mach; if a
# process still holds a venv DLL (glean_ffi.dll), that rebuild stops half done
# and later commands hang. Git Bash and an inherited PATH resolve python3 elsewhere.
python="/c/mozilla-build/python3/python3.exe"
[ -x "$python" ] || python=python3
# Both mozconfigs build with the project-owned Fenn branding; mirror it first.
"$python" "$project_root/scripts/Stage-FennBranding.py"
cd -- "$project_root/upstream/firefox"
exec "$python" mach "$@"
