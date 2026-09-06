#!/usr/bin/env bash
set -euo pipefail

destination=${1:?用法: container-stage.sh DESTDIR LIBDIR}
libdir=${2:?用法: container-stage.sh DESTDIR LIBDIR}

cmake -S /src -B /tmp/wetypex-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR="$libdir"
cmake --build /tmp/wetypex-build --parallel "${WETYPEX_BUILD_JOBS:-2}"
mkdir -p "$destination"
DESTDIR="$destination" cmake --install /tmp/wetypex-build
