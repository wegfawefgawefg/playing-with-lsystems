#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")"/.. && pwd)"
preset="${LSYSTEMS_PRESET:-release}"
build_dir="${repo_root}/build"

if [ "${preset}" = "dev" ]; then
    build_dir="${repo_root}/build-debug"
fi

configure() {
    cmake --preset "${preset}"
}

if ! configure; then
    rm -f "${build_dir}/CMakeCache.txt"
    rm -rf "${build_dir}/CMakeFiles"
    configure
fi

if [ "${1:-}" = "--configure-only" ]; then
    exit 0
fi

cmake --build --preset "${preset}" -j
