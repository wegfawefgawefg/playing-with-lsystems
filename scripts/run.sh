#!/usr/bin/env bash
set -euo pipefail

if [ -n "${DISPLAY:-}" ] && [ -z "${SDL_VIDEODRIVER:-}" ]; then
    export SDL_VIDEODRIVER=x11
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")"/.. && pwd)"
preset="${LSYSTEMS_PRESET:-release}"
binary_dir="build"

if [ "${preset}" = "dev" ]; then
    binary_dir="build-debug"
fi

"${repo_root}/scripts/build.sh"
"${repo_root}/${binary_dir}/lsystems" "$@"
