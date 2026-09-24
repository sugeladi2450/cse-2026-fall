#!/bin/sh
set -eu
[ "$#" -eq 2 ] || { echo "usage: $0 HOST:PORT MOUNT" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$2"
exec "${NANOFS_BUILD_DIR:-$root/build}/nanofs/nanofs_fuse" --server "$1" -f "$2"
