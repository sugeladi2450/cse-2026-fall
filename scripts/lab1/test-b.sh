#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=${NANOFS_BUILD_DIR:-"$root/build"}
cmake -S "$root" -B "$build" -DNANOFS_STAGE_B=ON
cmake --build "$build" -j4
ctest --test-dir "$build" --output-on-failure --no-tests=error
