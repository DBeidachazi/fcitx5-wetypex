#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
destination=${1:?用法: packaging/stage.sh DESTDIR}
build_dir=${WETYPEX_BUILD_DIR:-$project_root/build/release}

cmake -S "$project_root" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$build_dir" --parallel "${WETYPEX_BUILD_JOBS:-2}"
DESTDIR="$destination" cmake --install "$build_dir"
