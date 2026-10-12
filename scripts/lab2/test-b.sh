#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=${NANOFS_BUILD_DIR:-"$root/build"}
timeout=${CTEST_TIMEOUT:-10}
cmake -S "$root" -B "$build" -DNANOFS_STAGE_B=ON -DNANOFS_LAB2B=ON
cmake --build "$build" -j4
ctest --test-dir "$build" --output-on-failure --no-tests=error --timeout "$timeout"
