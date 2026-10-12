#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=${NANOFS_BUILD_DIR:-"$root/build"}
cmake -S "$root" -B "$build" -DNANOFS_STAGE_B=ON -DNANOFS_LAB2B=ON \
  -DNANOFS_REQUIRE_FUSE=ON
cmake --build "$build" -j4 --target nanofs_fuse
python3 "$root/scripts/lab2/fuse_acceptance.py" "$build/nanofs/nanofs_fuse"
