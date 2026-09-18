#!/bin/sh
set -eu
[ "$#" -eq 1 ] || { echo "usage: $0 MOUNT" >&2; exit 2; }
case "$(uname -s)" in
 Darwin) diskutil unmount "$1" ;;
 *) fusermount3 -u "$1" ;;
esac
