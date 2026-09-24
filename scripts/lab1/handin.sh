#!/bin/sh
set -eu
[ "$#" -eq 2 ] || { echo "usage: $0 a|b STUDENT_ID" >&2; exit 2; }
case "$2" in *[!A-Za-z0-9_-]*|'') echo "invalid student id" >&2; exit 2;; esac
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
case "$1" in
 a) set -- a "$2" nanofs/src/fs.cc ;;
 b) set -- b "$2" nanofs/src/fs.cc nanofs/src/inode.cc nanofs/src/handle.cc nanofs/include/nanofs/remote_fs.h ;;
 *) echo "stage must be a or b" >&2; exit 2;;
esac
archive="handin-lab1$1-$2.tgz"; shift 2
[ ! -e "$archive" ] || { echo "archive exists: $archive (rename it before resubmitting)" >&2; exit 1; }
tar -czf "$archive" "$@"
printf 'Created %s\n' "$root/$archive"
