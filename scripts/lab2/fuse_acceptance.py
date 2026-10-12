#!/usr/bin/env python3
"""Optional end-to-end concurrent workload over a real nanoFS FUSE mount."""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time


def wait_for_mount(mountpoint: pathlib.Path, proc: subprocess.Popen, timeout: float = 8.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"nanofs_fuse exited with status {proc.returncode}")
        if os.path.ismount(mountpoint):
            return
        time.sleep(0.1)
    raise RuntimeError("FUSE mount did not become ready")


def unmount(mountpoint: pathlib.Path) -> None:
    for command in (["fusermount3", "-u", str(mountpoint)],
                    ["fusermount", "-u", str(mountpoint)],
                    ["umount", str(mountpoint)]):
        if shutil.which(command[0]) and subprocess.run(command, check=False).returncode == 0:
            return


def concurrent_creates(mountpoint: pathlib.Path, workers: int = 12) -> None:
    barrier = threading.Barrier(workers)
    failures = []

    def write_one(index: int) -> None:
        try:
            payload = (f"worker-{index}:" * 64).encode()
            barrier.wait()
            (mountpoint / f"f{index}.txt").write_bytes(payload)
        except BaseException as error:
            failures.append(error)

    threads = [threading.Thread(target=write_one, args=(i,)) for i in range(workers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if failures:
        raise failures[0]
    for index in range(workers):
        expected = (f"worker-{index}:" * 64).encode()
        actual = (mountpoint / f"f{index}.txt").read_bytes()
        if actual != expected:
            raise AssertionError(f"f{index}.txt is missing or corrupt")
    print(f"phase1 OK: {workers} concurrent files are present and intact")


def no_torn_read(mountpoint: pathlib.Path, iterations: int = 400) -> None:
    path = mountpoint / "shared.txt"
    value_a, value_b = b"A" * 512, b"B" * 512
    path.write_bytes(value_a)
    stop = threading.Event()
    torn = []

    def writer() -> None:
        descriptor = os.open(path, os.O_WRONLY)
        try:
            for index in range(iterations):
                os.lseek(descriptor, 0, os.SEEK_SET)
                os.write(descriptor, value_b if index & 1 else value_a)
        finally:
            os.close(descriptor)
            stop.set()

    def reader() -> None:
        while not stop.is_set():
            value = path.read_bytes()
            if value not in (value_a, value_b):
                torn.append(value)
                return

    read_thread = threading.Thread(target=reader)
    write_thread = threading.Thread(target=writer)
    read_thread.start()
    write_thread.start()
    write_thread.join()
    read_thread.join()
    if torn:
        raise AssertionError(f"reader observed a torn value of {len(torn[0])} bytes")
    print("phase2 OK: concurrent reads observe only complete values")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} NANOSF_FUSE_BINARY", file=sys.stderr)
        return 2
    binary = pathlib.Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print(f"missing FUSE binary: {binary}", file=sys.stderr)
        return 2
    temporary = pathlib.Path(tempfile.mkdtemp(prefix="cse-lab2-fuse."))
    mountpoint = temporary / "mnt"
    mountpoint.mkdir()
    log_path = temporary / "fuse.log"
    process = None
    try:
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                [str(binary), "--format", str(mountpoint), "-f"],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            wait_for_mount(mountpoint, process)
            concurrent_creates(mountpoint)
            no_torn_read(mountpoint)
            print("FUSE ACCEPTANCE PASS")
        return 0
    except BaseException as error:
        print(f"FUSE ACCEPTANCE FAIL: {error}; logs preserved at {temporary}", file=sys.stderr)
        return 1
    finally:
        if process is not None:
            unmount(mountpoint)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
            if process.returncode not in (None, 0):
                print(f"nanofs_fuse exit status: {process.returncode}; log: {log_path}", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
