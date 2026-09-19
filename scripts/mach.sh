#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export MOZBUILD_STATE_PATH="$(cygpath -m "$project_root/.cache/mozbuild")"
# Read-mostly toolchains may be shared between checkouts; see Enter-DevEnvironment.ps1.
shared_cache="${FIREFOX_WINUI_SHARED_CACHE:-$project_root/.cache}"
export CARGO_HOME="$(cygpath -m "$shared_cache/cargo")"
export RUSTUP_HOME="$(cygpath -m "$shared_cache/rustup")"
export PIP_CACHE_DIR="$(cygpath -m "$shared_cache/pip")"
export SCCACHE_DIR="$(cygpath -m "$project_root/.cache/sccache")"
export MOZCONFIG="$(cygpath -m "$project_root/configs/mozconfig")"
export PATH="$(cygpath -u "$CARGO_HOME")/bin:$PATH"
cd -- "$project_root/upstream/firefox"
exec python3 mach "$@"
