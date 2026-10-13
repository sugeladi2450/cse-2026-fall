# Lab 2: Make nanoFS atomic

Lab 2 builds on your completed Lab1A and Lab1B filesystem. The `lab2`
release branch contains both stages. Merge it into your existing working
branch, then complete:

- **[Lab2A](lab2a.md)**: add redo metadata journaling so mutating operations are
  all-or-nothing across a crash.
- **[Lab2B](lab2b.md)**: add fine-grained locking so concurrent operations
  behave in a before-or-after order. It uses your Lab2A work.

## Development container

Reuse the Lab1 image, which provides Ubuntu 24.04, GCC 14, CMake and FUSE 3.
The repository is mounted at `/work`, so edits on your computer are available
inside the container. If your Lab1 container mounts the same working directory,
continue using it after merging Lab2; no new image or dependencies are needed.

If you need a new container, pull the published Lab1 image and give it a
local Lab2 tag. Run these commands on the host from your Lab2 repository root:

```sh
docker pull chilsome/cse-2026-lab1:20260918
docker tag chilsome/cse-2026-lab1:20260918 cse-2026-lab2
```

Alternatively, build the same Dockerfile locally:

```sh
docker build -t cse-2026-lab2 .
```

Create a container with FUSE support using the local Lab2 tag:

**macOS / WSL Ubuntu (Bash or Zsh):**

```sh
docker run --name cse-2026-lab2-fuse -it \
  --device /dev/fuse --cap-add SYS_ADMIN \
  --security-opt apparmor=unconfined \
  -e NANOFS_BUILD_DIR=/work/build-linux \
  -v "$PWD:/work" cse-2026-lab2
```

**Windows PowerShell (copy the complete single line):**

```powershell
docker run --name cse-2026-lab2-fuse -it --device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor=unconfined -e NANOFS_BUILD_DIR=/work/build-linux -v "${PWD}:/work" cse-2026-lab2
```

If Docker reports that `/dev/fuse` is unavailable, create the container
without FUSE instead. All required tests, the bonus benchmark and handin still
work; skip the optional FUSE acceptance test in this container.

**macOS / WSL Ubuntu:**

```sh
docker run --name cse-2026-lab2-fuse -it \
  -e NANOFS_BUILD_DIR=/work/build-linux \
  -v "$PWD:/work" cse-2026-lab2
```

**Windows PowerShell:**

```powershell
docker run --name cse-2026-lab2-fuse -it -e NANOFS_BUILD_DIR=/work/build-linux -v "${PWD}:/work" cse-2026-lab2
```

If the failed startup left a container with that name, choose a different
`--name` and use it in the start/exec commands below.

Return to this container with `docker start -ai cse-2026-lab2-fuse`, or open
another shell while it is running with `docker exec -it cse-2026-lab2-fuse bash`.
Inside the container, run the stage test scripts from `/work`.

You can also keep using your existing Lab1 container if it mounts this working
tree: use `docker start -ai cse-2026-lab1-fuse` or, while it is running,
`docker exec -it cse-2026-lab1-fuse bash`. If you switch working directories,
create a container from the new repository root. See the
[Lab1 Docker setup](../lab1/lab1.md#docker-container) for Docker installation
and WSL setup.

Course tips and answers to student questions are in the [Lab2 Q&A](https://docs.qq.com/doc/DYVBVdW1OUFpMUG5U?nlc=1).

## Get Lab2 once

Commit your completed Lab1B work, then merge the single Lab2 release:

```sh
git add nanofs/src/fs.cc nanofs/src/inode.cc nanofs/src/handle.cc \
  nanofs/include/nanofs/remote_fs.h
git commit -m "Complete Lab1"
git fetch https://ipads.sjtu.edu.cn:1312/lab/cse-2026-fall.git lab2
git merge FETCH_HEAD
```

If your Lab1 work is already committed, skip the first two commands.
Merge into your existing working
branch so you keep your Lab1 implementation. Commit or stash other changes
before merging, and preserve your implementation when resolving conflicts.

Complete Lab2A and commit a local checkpoint, then continue Lab2B in the
same working branch; there is no second release merge. Run the stage-specific
tests as you work. After completing both stages, submit Lab2 only once:

| Stage | Test |
|---|---|
| Lab2A journal | `scripts/lab2/test-a.sh` |
| Lab2B locking | `scripts/lab2/test-b.sh` |

After both stages pass, follow the [single Lab2 handin instructions](#grading-and-handin).

## Grading and handin

Pass the public tests and follow the behavior specified in the Lab2A/Lab2B
guides and headers. Hidden tests check the same requirements to discourage
programming specifically to the test cases. The optional bonus benchmark does
not replace correctness tests.

The stage scripts run unit tests and integration tests for journaling under
`TxFs`, simulated power-loss recovery, and concurrent `LockedFs` operations.
These tests do not require FUSE. You can also run the optional real-mount
acceptance test with `scripts/lab2/fuse-acceptance.sh`; see
[Lab2B testing](lab2b.md#compile-and-test). FUSE acceptance does not contribute
to the base score.

After completing both stages, package your implementation once. Run this
command inside the container from `/work`:

```sh
scripts/lab2/handin.sh YOUR_STUDENT_ID
```

Keep your completed Lab1B implementation. The archive contains these six files:

```text
nanofs/src/fs.cc
nanofs/src/inode.cc
nanofs/src/handle.cc
nanofs/include/nanofs/remote_fs.h
nanofs/src/journal.cc
nanofs/src/locked_fs.cc
```

Only these six files are accepted for grading. Keep the tests and provided
components unchanged.

The script creates `handin-lab2-YOUR_STUDENT_ID.tgz` in the repository root.
Check its contents with:

```sh
tar -tzf handin-lab2-YOUR_STUDENT_ID.tgz
```

Upload that `.tgz` file to the single Lab2 assignment on Canvas when it
opens. If an archive with the same name already exists, rename it before
running `handin.sh` again.
