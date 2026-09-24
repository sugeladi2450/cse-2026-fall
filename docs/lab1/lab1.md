# Lab 1: A filesystem for nanoCoder

## Get Ready

### Lab 1 Introduction

In Lab 1, you will implement an inode-based filesystem and then make it accessible
over the network. The lab has two parts, released separately:

- **Lab1A**: implement a single-machine filesystem.
- **[Lab1B](lab1b.md)**: add remote file access and handle RPC failures using
  your Lab1A implementation. Follow its upgrade instructions if you have finished A.

Start by setting up the environment below. Then follow the instructions in
[Lab1A](lab1a.md).

### Get Source Code

If you already have a Lab1A checkout, follow [Get the Lab1B Code](lab1b.md#get-the-lab1b-code)
to preserve your implementation. For a fresh checkout:

```sh
git clone -b lab1b https://github.com/33chilsome/cse-lab1.git cse-2026-fall
cd cse-2026-fall
```

The dependencies are included in the repository; no Git submodules are needed.

### Docker Container

We use a Docker container to compile the code and run the tests. The Dockerfile
provides Ubuntu 24.04, GCC 14, CMake and FUSE 3.

If you have not installed Docker, follow the official instructions for
[macOS](https://docs.docker.com/desktop/setup/install/mac-install/) or
[Windows](https://docs.docker.com/desktop/setup/install/windows-install/).
Windows users should use Docker Desktop's Linux containers with the WSL 2 backend.
If you run Docker commands from Ubuntu, also enable
[WSL 2 integration](https://docs.docker.com/desktop/features/wsl/) for Ubuntu.
Start Docker Desktop before continuing.

**Run the setup commands from the repository root, outside the container.**
Use your macOS terminal, WSL Ubuntu terminal, or Windows PowerShell, and choose
the matching `docker run` command below. After installing Docker, restart your
terminal application (or VS Code) so that it can find the `docker` command.

You have two choices to get the image:

- **[Recommended] Pull the prepared image from
  [Docker Hub](https://hub.docker.com/r/chilsome/cse-2026-lab1):**

```sh
docker pull chilsome/cse-2026-lab1:20260918
docker tag chilsome/cse-2026-lab1:20260918 cse-2026-lab1
```

Docker automatically selects the AMD64 or ARM64 image for your computer.

- Build the image locally if you prefer:

```sh
docker build -t cse-2026-lab1 .
```

After pulling or building the image, create a container with FUSE support:

**macOS / WSL Ubuntu (Bash or Zsh):**

```sh
docker run --name cse-2026-lab1-fuse -it \
  --device /dev/fuse --cap-add SYS_ADMIN \
  --security-opt apparmor=unconfined \
  -e NANOFS_BUILD_DIR=/work/build-linux \
  -v "$PWD:/work" cse-2026-lab1
```

**Windows PowerShell (copy the complete single line):**

```powershell
docker run --name cse-2026-lab1-fuse -it --device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor=unconfined -e NANOFS_BUILD_DIR=/work/build-linux -v "${PWD}:/work" cse-2026-lab1
```

You only need to create the container once. The repository is mounted at `/work`:
changes made in VS Code on your computer will also appear inside the container.
Your prompt should now look like this:

```text
root@xxxxxxxx:/work#
```

You can edit code outside the container, but **compile the code and run the tests
inside it**. Unless stated otherwise, the commands in both guides run from `/work`
inside this container. Build files are stored in `/work/build-linux`.

Type `exit` or press `Ctrl+D` to leave the shell. To return to the container, run
this command on your computer:

```sh
docker start -ai cse-2026-lab1-fuse
```

To open another shell while the container is running, run this on your computer:

```sh
docker exec -it cse-2026-lab1-fuse bash
```

## Compile Code and Run Tests

We provide unit tests for individual functions and integration tests that mount
your filesystem and perform file operations through it.

### Unit Tests

Inside the container, execute:

```sh
cd /work
bash scripts/lab1/test-a.sh
```

This script configures the project, compiles the code and runs all Lab1A unit
tests. Run it again after changing your implementation.

Before you implement any functions, **33 tests should pass and 19 should fail**.
The passing tests check code that we have provided. The failures are expected;
they do not mean your Docker environment is broken.

You can also rebuild and run one test suite while working on a function. For
example, to run the block-addressing test:

```sh
cmake --build "$NANOFS_BUILD_DIR" -j4
ctest --test-dir "$NANOFS_BUILD_DIR" -R '^LogicalToPhysical\.' --output-on-failure
```

Run the full unit test script at least once before using this command, so that
CMake has configured the build directory. The task descriptions list the test
suites for each part. You can read their source in `nanofs/test/` to understand a
failure.

### Integration Tests

After completing Lab1A, run these commands in the FUSE-enabled container:

```sh
scripts/lab1/integration_test.sh a quick
scripts/lab1/integration_test.sh a stress --seed 1
```

The script builds the mount program, creates a temporary filesystem and compares
its behavior with an ordinary directory. The quick test runs 200 operations;
the stress test runs 8000 and covers indirect blocks, block reuse and nearly full
storage. On success, the last line of the stress test is:

```text
PASS stage=a profile=stress seed=1
```

If configuration or compilation fails, check the error printed in the terminal.
If mounting or the workload fails, the script prints `FAIL preserved_logs=...`.
Read the workload error in the terminal and the filesystem output in `fuse.log`
in that directory. For a mount failure, also check the container options above.

To try commands such as `mkdir` and `cat` yourself, see
[Playing with Your Filesystem](lab1a.md#playing-with-your-filesystem).
To run nanoCoder on your completed filesystem, follow
[Running nanoCoder on nanoFS](nanocoder-demo.md).

## Grading

For Lab1A, your implementation must pass all 52 unit tests and the stress integration test
above. Do not reduce the operation count with `--ops` for grading. Save the PASS
line, including the seed, from your integration test run. Deadlines and score
weights will be announced on the course page.

You may add tests or temporarily change tests while debugging, but we will use
the original course tests when grading. Submit only the file specified below.

Lab1B has its own [tests and submission requirements](lab1b.md#grading-and-handin).

## Handin

For Lab1A, inside the container, execute the following command from `/work`, replacing
`YOUR_STUDENT_ID` with your student ID:

```sh
scripts/lab1/handin.sh a YOUR_STUDENT_ID
```

The script creates `handin-lab1a-YOUR_STUDENT_ID.tgz` in the repository root. It
contains only `nanofs/src/fs.cc`. The archive is also visible in the repository
on your computer; submit the `.tgz` archive to the Lab1A assignment on Canvas.

If an archive with the same name already exists, rename it before running the
script again. Keep your Lab1A implementation for the later Lab1B release.
