#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
python3 "$root/scripts/lab2/check_compat.py" b "$root/nanofs/src/fs.cc"
