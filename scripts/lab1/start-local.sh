#!/bin/sh
set -eu
[ "$#" -ge 2 ] || { echo "usage: $0 MOUNT IMAGE [--format]" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mnt=$1; image=$2; shift 2
mkdir -p "$mnt"
exec "${NANOFS_BUILD_DIR:-$root/build}/nanofs/nanofs_fuse" --image "$image" "$@" -f "$mnt"
