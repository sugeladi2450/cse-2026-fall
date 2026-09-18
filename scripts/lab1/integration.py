#!/usr/bin/env python3
"""Run the provided mirror workload against a local or RPC FUSE mount."""
import argparse
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('stage', choices=['a'])
    parser.add_argument('profile', choices=['quick', 'stress'], nargs='?', default='quick')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--ops', type=int, help='debugging only; omit for grading')
    args = parser.parse_args()
    build = Path(os.environ.get('NANOFS_BUILD_DIR', ROOT / 'build')).resolve()
    subprocess.run(['cmake', '-S', str(ROOT), '-B', str(build),
                    '-DNANOFS_REQUIRE_FUSE=ON',
                    '-DNANOFS_STAGE_B=' + ('ON' if args.stage == 'b' else 'OFF')], check=True)
    targets = ['nanofs_fuse'] + (['nanofsd'] if args.stage == 'b' else [])
    subprocess.run(['cmake', '--build', str(build), '--target', *targets, '-j4'], check=True)
    temp = Path(tempfile.mkdtemp(prefix='cse-lab1-'))
    mount = temp / 'mnt'; mount.mkdir()
    mirror = temp / 'mirror'; mirror.mkdir()
    processes = []
    logs = []
    passed = False
    try:
        geometry = ['--block-size', '4096', '--num-blocks', '4096', '--num-inodes', '256']
        def start(command, name):
            log = (temp / name).open('w'); logs.append(log)
            proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            processes.append(proc)
            return proc
        command = [str(build / 'nanofs/nanofs_fuse'), '-f', '--log-level', 'warn',
                   '-o', 'entry_timeout=0', '-o', 'attr_timeout=0', '-o', 'negative_timeout=0']
        if args.stage == 'a':
            command += ['--image', str(temp / 'disk.img'), '--format', *geometry]
        else:
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
            server = start([str(build / 'nanofs/nanofsd'), '--port', str(port),
                            '--image', str(temp / 'disk.img'), '--format',
                            '--log-level', 'warn', *geometry], 'server.log')
            for _ in range(100):
                if server.poll() is not None:
                    raise RuntimeError('server exited before becoming ready')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.1):
                        break
                except OSError:
                    time.sleep(.1)
            else:
                raise RuntimeError('server startup timeout')
            command += ['--server', f'127.0.0.1:{port}']
        fuse = start([*command, str(mount)], 'fuse.log')
        for _ in range(100):
            if fuse.poll() is not None:
                raise RuntimeError('FUSE exited before mounting')
            if os.path.ismount(mount):
                break
            time.sleep(.1)
        else:
            raise RuntimeError('mount timeout')
        workload = [sys.executable, str(ROOT / 'nanofs/stress/tests/mirror_ops.py'),
                    '--mount', str(mount), '--mirror', str(mirror), '--profile', args.profile,
                    '--seed', str(args.seed), *geometry]
        if args.ops is not None:
            workload += ['--ops', str(args.ops)]
        subprocess.run(workload, check=True)
        subprocess.run([str(ROOT / 'scripts/lab1/stop-fs.sh'), str(mount)], check=True)
        fuse.wait(timeout=15)
        if fuse.returncode:
            raise RuntimeError('FUSE failed during clean shutdown')
        if args.stage == 'b':
            server.terminate(); server.wait(timeout=15)
            if server.returncode:
                raise RuntimeError('server failed during clean shutdown')
        passed = True
    finally:
        if os.path.ismount(mount):
            subprocess.run([str(ROOT / 'scripts/lab1/stop-fs.sh'), str(mount)], check=False)
        still_mounted = os.path.ismount(mount)
        if still_mounted:
            passed = False
            print(f"Unmount manually before stopping these processes: {mount}", file=sys.stderr)
        for proc in ([] if still_mounted else reversed(processes)):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    # Do not force-kill a process while its filesystem is still mounted.
                    passed = False
                    print(f'Cleanup needs attention: pid={proc.pid}', file=sys.stderr)
        for log in logs:
            log.close()
        if passed and not os.path.ismount(mount):
            shutil.rmtree(temp)
        else:
            print(f'FAIL preserved_logs={temp}', file=sys.stderr)
    if not passed:
        return 1
    print(f'PASS stage={args.stage} profile={args.profile} seed={args.seed}')
    return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.SubprocessError, OSError) as exc:
        print(f'FAIL {exc}', file=sys.stderr)
        sys.exit(1)
