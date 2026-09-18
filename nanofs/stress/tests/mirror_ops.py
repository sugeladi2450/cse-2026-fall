#!/usr/bin/env python3
import argparse
import os
import random
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class RunResult:
    seed: int
    ops: int
    profile: str


@dataclass(frozen=True)
class Capacity:
    """NanoFS image geometry the workload must stay within.

    block_size/num_blocks/num_inodes mirror the values passed to
    nanofs_fuse. Used to bound the working set so the run stays near the
    image's real capacity without going past it: the host-backed mirror
    would not hit ENOSPC/inode exhaustion, so an unbounded run could fail
    spuriously the moment NanoFS fills up.
    """

    num_blocks: int
    num_inodes: int
    block_size: int


@dataclass(frozen=True)
class Limits:
    """Caps the workload keeps allocated so worst-case usage stays under the image.

    max_regular_files bounds data-block usage; max_live_dirs and
    max_live_inodes bound directory and inode-table usage. See
    _capacity_limits for the block-budget derivation.
    """

    max_regular_files: int
    max_live_dirs: int
    max_live_inodes: int
    max_big_files: int


# --- workload tuning (shared by the block budget and the actions, so the
# --- bound always matches what the actions can actually allocate) ---

# Regular-file write window per profile: writes land in [0, off] with size in
# [1, size], so a live file can touch at most ceil((off+size)/bs) data blocks.
_WRITE_OFF = {"quick": 8192, "stress": 65536}
_WRITE_SIZE = {"quick": 4096, "stress": 16384}
# Truncate/read reach further (holes read as zero); truncate-grow is sparse and
# allocates nothing, so it does not enter the block budget.
_TRUNC_MAX = {"quick": 16384, "stress": 131072}
_READ_OFF = {"quick": 16384, "stress": 131072}
_READ_SIZE = {"quick": 8192, "stress": 32768}

_DIRENT_SIZE = 64                 # sizeof(DirEntry) (nanofs/include/nanofs/fs.h)
_BLOCK_ID_SIZE = 4                # sizeof(block_id) == uint32_t
_NDIRECT = 100                    # direct pointers per inode (nanofs/include/nanofs/inode.h)
_INODE_RESERVE = 16               # inodes kept spare (root + margin), never allocated
_FILL_TARGET = 0.90               # aim to fill this fraction of usable data blocks
_MAX_LIVE_DIRS = 32               # cap on live directories (keeps dir-block reserve small)

# Big files exercise the single/double indirect block paths, which the small
# regular files (<= ~20 blocks << NDIRECT=100) never reach. Each big file is
# written only at these fixed, block-aligned logical indices:
#   50   -> direct               (index < NDIRECT)
#   150  -> single indirect      (NDIRECT <= index < NDIRECT + bs/4)
#   1200 -> double indirect      (index >= NDIRECT + bs/4, for bs=4096)
# The writes are logically sparse (64 bytes at a high offset), but on macOS the
# FUSE kernel zero-fills the hole before EOF, so a big file physically occupies
# every block up to its top tier (~1200 data blocks), not the handful a sparse
# file would take. Keep the reserve dense so the same capacity model is safe on
# both macOS and Linux.
_BIG_TIER_BLOCKS = (50, 150, 1200)
_BIG_WRITE_SIZE = 64
_BIG_FILES = 1
# Dense worst case per big file: every data block through the highest tier, plus
# the indirect metadata blocks along the deepest path (single root + double root
# + double L1).
_BIG_FILE_MAX_BLOCKS = max(_BIG_TIER_BLOCKS) + 1 + 3


def random_bytes(seed: int, size: int) -> bytes:
    rng = random.Random(seed)
    return bytes(rng.randrange(0, 256) for _ in range(size))


def compare_trees(left: Path, right: Path) -> None:
    left_dirs = _tree_dirs(left)
    right_dirs = _tree_dirs(right)
    if left_dirs != right_dirs:
        raise AssertionError(f"directory mismatch: {left_dirs} != {right_dirs}")
    left_files = _tree_files(left)
    right_files = _tree_files(right)
    if left_files != right_files:
        raise AssertionError(f"tree mismatch: {left_files} != {right_files}")
    for rel in left_files:
        lpath = left / rel
        rpath = right / rel
        if lpath.read_bytes() != rpath.read_bytes():
            raise AssertionError(f"content mismatch: {rel}")


def run_profile(
    mount: Path,
    mirror: Path,
    seed: int,
    ops: int,
    profile: str,
    show_progress: bool = False,
    progress_factory=None,
    capacity: "Capacity | None" = None,
) -> RunResult:
    if profile not in {"quick", "stress"}:
        raise ValueError(f"unknown profile: {profile}")
    rng = random.Random(seed)
    dirs = [Path(".")]
    files: list[Path] = []
    big_files: list[Path] = []
    big_write_count = 0
    limits = _capacity_limits(profile, capacity)
    bs = capacity.block_size if capacity is not None else 4096
    compare_every = 25 if profile == "quick" else 500
    progress = None
    if show_progress:
        if progress_factory is None:
            progress_factory = _progress_factory
        progress = progress_factory(total=ops, desc="mirror_ops", unit="op")

    try:
        for index in range(ops):
            action = _choose_action(rng, files, dirs, big_files, profile, limits, capacity)
            if action == "mkdir":
                rel = _new_dir_name(rng, dirs)
                _mkdir(mount, rel)
                _mkdir(mirror, rel)
                dirs.append(rel)
            elif action == "rmdir":
                rel = rng.choice(_removable_dirs(dirs, files, big_files))
                os.rmdir(mount / rel)
                os.rmdir(mirror / rel)
                dirs.remove(rel)
            elif action == "create":
                rel = _new_file_name(rng, dirs, files)
                _write_at(mount, rel, 0, b"")
                _write_at(mirror, rel, 0, b"")
                files.append(rel)
            elif action == "write":
                rel = rng.choice(files)
                size = rng.randint(1, _WRITE_SIZE[profile])
                offset = rng.randint(0, _WRITE_OFF[profile])
                data = random_bytes(seed * 1_000_003 + index, size)
                _write_at(mount, rel, offset, data)
                _write_at(mirror, rel, offset, data)
            elif action == "big_write":
                big_write_count = _big_write(
                    mount, mirror, seed, big_files, big_write_count, bs)
            elif action == "truncate":
                rel = rng.choice(files)
                size = rng.randint(0, _TRUNC_MAX[profile])
                os.truncate(mount / rel, size)
                os.truncate(mirror / rel, size)
            elif action == "read":
                rel = rng.choice(files)
                offset = rng.randint(0, _READ_OFF[profile])
                size = rng.randint(0, _READ_SIZE[profile])
                if _read_at(mount, rel, offset, size) != _read_at(mirror, rel, offset, size):
                    raise AssertionError(f"read mismatch: {rel} off={offset} size={size}")
            elif action == "unlink":
                rel = rng.choice(files)
                os.unlink(mount / rel)
                os.unlink(mirror / rel)
                files.remove(rel)

            if progress is not None:
                progress.update(1)
            if index % compare_every == 0:
                compare_trees(mount, mirror)
    finally:
        if progress is not None:
            progress.close()

    compare_trees(mount, mirror)
    return RunResult(seed=seed, ops=ops, profile=profile)


def main() -> int:
    parser = argparse.ArgumentParser(description="Mirror NanoFS FUSE operations against a local directory.")
    parser.add_argument("--mount", required=True, type=Path, help="NanoFS mount point")
    parser.add_argument("--mirror", required=True, type=Path, help="local mirror directory")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--ops", type=int, default=None)
    parser.add_argument("--profile", choices=["quick", "stress"], default="quick")
    parser.add_argument("--num-blocks", type=int, default=None, help="NanoFS image block count (bounds the working set)")
    parser.add_argument("--num-inodes", type=int, default=None, help="NanoFS image inode count (bounds the working set)")
    parser.add_argument("--block-size", type=int, default=4096, help="NanoFS image block size")
    args = parser.parse_args()

    if (args.num_blocks is None) != (args.num_inodes is None):
        parser.error("--num-blocks and --num-inodes must be given together")
    capacity = None
    if args.num_blocks is not None:
        capacity = Capacity(args.num_blocks, args.num_inodes, args.block_size)

    ops = args.ops if args.ops is not None else (200 if args.profile == "quick" else 8000)
    _prepare_empty_dir(args.mirror)
    result = run_profile(
        args.mount, args.mirror, args.seed, ops, args.profile,
        show_progress=False, capacity=capacity,
    )
    print(f"PASS test=mirror_ops profile={result.profile} seed={result.seed} ops={result.ops}")
    return 0


def _tree_files(root: Path) -> list[Path]:
    out: list[Path] = []
    for path in root.rglob("*"):
        if path.is_file():
            out.append(path.relative_to(root))
    return sorted(out)


def _tree_dirs(root: Path) -> list[Path]:
    out: list[Path] = []
    for path in root.rglob("*"):
        if path.is_dir():
            out.append(path.relative_to(root))
    return sorted(out)


def _ceil_div(a: int, b: int) -> int:
    return -(-a // b)


def _metadata_blocks(capacity: Capacity) -> int:
    """Blocks NanoFS reserves before the data region, mirroring InodeManager::format.

    Layout: [super | inode table | inode bitmap | block bitmap | data]. The
    data region is num_blocks minus this; every file/dir data block, inode
    struct block, and indirect block is drawn from it.
    """
    bs, nb, ni = capacity.block_size, capacity.num_blocks, capacity.num_inodes
    inode_table = _ceil_div((ni + 1) * _BLOCK_ID_SIZE, bs)
    inode_bitmap = _ceil_div(_ceil_div(ni + 1, 8), bs)
    block_bitmap = _ceil_div(_ceil_div(nb, 8), bs)
    return 1 + inode_table + inode_bitmap + block_bitmap


def _blocks_per_regular_file(profile: str, bs: int) -> int:
    """Max data blocks a regular file can hold: content blocks + its inode struct block."""
    return _ceil_div(_WRITE_OFF[profile] + _WRITE_SIZE[profile], bs) + 1


def _capacity_limits(profile: str, capacity: "Capacity | None") -> "Limits | None":
    """Derive live-set caps so worst-case block usage stays under _FILL_TARGET of usable.

    Returns None when no capacity is given (unbounded: the two-local-dir unit
    test). Otherwise splits a fill-target block budget across three reserves
    (the big indirect files, all directories with their inode-struct and entry
    blocks, and the regular files), so that even if every live file fills its
    whole write window the total never reaches ENOSPC. Each per-file cost
    includes the one data block NanoFS spends on the inode struct itself (see
    allocate_inode).
    """
    if capacity is None:
        return None
    bs = capacity.block_size
    usable = capacity.num_blocks - _metadata_blocks(capacity)
    budget = int(usable * _FILL_TARGET)
    max_live_inodes = max(2, capacity.num_inodes - _INODE_RESERVE)
    max_live_dirs = min(_MAX_LIVE_DIRS, max_live_inodes // 2)

    # Directories: one inode-struct block each, plus entry blocks. Every live
    # file/dir occupies one entry somewhere, so total entry bytes <=
    # max_live_inodes * DIRENT_SIZE; the extra max_live_dirs covers per-dir
    # rounding when entries are spread thin.
    dir_reserve = max_live_dirs + _ceil_div(max_live_inodes * _DIRENT_SIZE, bs) + max_live_dirs
    per_regular = _blocks_per_regular_file(profile, bs)
    per_big = _BIG_FILE_MAX_BLOCKS + 1
    max_big_files = _BIG_FILES
    while max_big_files > 0 and budget - max_big_files * per_big - dir_reserve < per_regular:
        max_big_files -= 1
    big_reserve = max_big_files * per_big
    reg_budget = budget - big_reserve - dir_reserve

    max_regular_files = max(1, reg_budget // per_regular)
    max_regular_files = min(max_regular_files, max_live_inodes - max_live_dirs - max_big_files)
    return Limits(max_regular_files, max_live_dirs, max_live_inodes, max_big_files)


def _live_inodes(files: list[Path], dirs: list[Path], big_files: list[Path]) -> int:
    # dirs includes Path('.') (the root), which owns no allocatable inode here.
    return len(files) + len(big_files) + (len(dirs) - 1)


def _removable_dirs(dirs: list[Path], files: list[Path], big_files: list[Path]) -> list[Path]:
    """Return non-root directories with no children (safe to rmdir)."""
    occupied: set[Path] = set()
    for path in files + big_files:
        occupied.update(path.parents)
    for d in dirs:
        if d != Path("."):
            occupied.update(d.parents)
    return [d for d in dirs if d != Path(".") and d not in occupied]


def _choose_action(
    rng: random.Random,
    files: list[Path],
    dirs: list[Path],
    big_files: list[Path],
    profile: str,
    limits: "Limits | None",
    capacity: "Capacity | None",
) -> str:
    if not files:
        return "create"
    weights = [
        ("write", 40),
        ("read", 22),
        ("truncate", 12),
        ("create", 10),
        ("unlink", 3 if profile == "quick" else 6),
        ("mkdir", 3),
        ("rmdir", 3),
    ]
    if capacity is not None and (limits is None or limits.max_big_files > 0):
        weights.append(("big_write", 4))
    total = sum(weight for _, weight in weights)
    pick = rng.randint(1, total)
    acc = 0
    action = "write"
    for candidate, weight in weights:
        acc += weight
        if pick <= acc:
            action = candidate
            break

    # Deterministic downgrades keep the run inside the NanoFS image and inside
    # the actual tree state. They depend only on the shared (files, dirs,
    # big_files) state, so mount and mirror stay in lockstep. Fallback is
    # "write" because files is non-empty on every weighted path.
    if limits is not None and action in ("create", "mkdir"):
        live = _live_inodes(files, dirs, big_files)
        at_inode_cap = live >= limits.max_live_inodes
        at_file_cap = action == "create" and len(files) >= limits.max_regular_files
        at_dir_cap = action == "mkdir" and (len(dirs) - 1) >= limits.max_live_dirs
        if at_inode_cap or at_file_cap or at_dir_cap:
            action = "write"
    if action == "rmdir" and not _removable_dirs(dirs, files, big_files):
        action = "write"
    return action


def _big_write(
    mount: Path,
    mirror: Path,
    seed: int,
    big_files: list[Path],
    count: int,
    bs: int,
) -> int:
    """Write a small chunk at a tiered logical offset to exercise indirect blocks.

    Lazily creates up to _BIG_FILES dedicated files, then cycles file and tier
    deterministically so the double-indirect path is reached over a run. Returns
    the incremented big-write counter. Both trees get identical bytes.
    """
    if len(big_files) < _BIG_FILES:
        rel = Path(f"big{len(big_files):02d}.bin")
        _write_at(mount, rel, 0, b"")
        _write_at(mirror, rel, 0, b"")
        big_files.append(rel)
    rel = big_files[count % len(big_files)]
    offset = _BIG_TIER_BLOCKS[count % len(_BIG_TIER_BLOCKS)] * bs
    data = random_bytes(seed * 7_000_003 + count, _BIG_WRITE_SIZE)
    _write_at(mount, rel, offset, data)
    _write_at(mirror, rel, offset, data)
    return count + 1


def _new_dir_name(rng: random.Random, dirs: list[Path]) -> Path:
    existing = set(dirs)
    while True:
        parent = rng.choice(dirs)
        name = f"d{rng.randrange(1_000_000):06d}"
        rel = parent / name if parent != Path(".") else Path(name)
        if rel not in existing:
            return rel


def _new_file_name(rng: random.Random, dirs: list[Path], files: list[Path]) -> Path:
    existing = set(files)
    while True:
        parent = rng.choice(dirs)
        name = f"f{rng.randrange(1_000_000):06d}.bin"
        rel = parent / name if parent != Path(".") else Path(name)
        if rel not in existing:
            return rel


def _mkdir(root: Path, rel: Path) -> None:
    (root / rel).mkdir()


def _write_at(root: Path, rel: Path, offset: int, data: bytes) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        os.lseek(fd, offset, os.SEEK_SET)
        os.write(fd, data)
    finally:
        os.close(fd)


def _read_at(root: Path, rel: Path, offset: int, size: int) -> bytes:
    with (root / rel).open("rb") as fh:
        fh.seek(offset)
        return fh.read(size)


def _prepare_empty_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)


if __name__ == "__main__":
    raise SystemExit(main())
