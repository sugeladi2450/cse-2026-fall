#!/usr/bin/env python3
"""Check the marked Lab1/Lab2 integration edits without matching comments."""

import argparse
import pathlib
import re
import sys


def code_without_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


def function(text: str, start: str, end: str) -> str:
    begin = text.find(start)
    finish = text.find(end, begin + len(start))
    if begin < 0 or finish < 0:
        raise ValueError(f"cannot locate {start}")
    return text[begin:finish]


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"missing: {message}", file=sys.stderr)
        raise SystemExit(1)


parser = argparse.ArgumentParser()
parser.add_argument("stage", choices=("a", "b"))
parser.add_argument("fs_cc", type=pathlib.Path)
args = parser.parse_args()
source = code_without_comments(args.fs_cc.read_text())

write_bytes = function(source, "FsLayer::write_bytes", "FsLayer::getattr")
require("BlockKind::Data" in write_bytes and "BlockKind::Meta" in write_bytes,
        "write_bytes must classify regular-file data and directory metadata")

if args.stage == "b":
    for name, end in (("FsLayer::make_node", "FsLayer::create"),
                      ("FsLayer::unlink", "FsLayer::rmdir"),
                      ("FsLayer::rmdir", "FsLayer::setattr")):
        body = function(source, name, end)
        # A Lab1 implementation may inspect directory entries without calling
        # lookup at all. Reject the common unsafe unqualified virtual call;
        # behavioral concurrency tests remain the authority for other styles.
        unsafe_lookup = re.search(r"(?<![\w.:])lookup\s*\(", body)
        require(not unsafe_lookup,
                f"{name.split('::')[-1]} must not re-enter virtual lookup")

print(f"Lab2{args.stage.upper()} fs.cc compatibility check passed")
