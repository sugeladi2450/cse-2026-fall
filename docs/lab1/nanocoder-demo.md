# Running nanoCoder on nanoFS

After completing Lab1A, you can use your filesystem as nanoCoder's workspace.
nanoCoder uses ordinary file operations, which FUSE forwards to your filesystem.
The application code is provided in [`nanocoder/`](../../nanocoder/).

This demo uses [`examples/hello.json`](../../examples/hello.json) to specify a
sequence of tool calls instead of asking a language model to choose them. The
file operations are real; no model service or API key is needed.

## Mount Your Filesystem

First pass the [Lab1A tests](lab1.md#unit-tests), then follow
[Playing with Your Filesystem](lab1a.md#playing-with-your-filesystem) to build and
mount nanoFS at `/tmp/cse-mnt`. Leave the mount program running in the first
terminal. Open a second shell in the same container as described there.

If you already finished that section and unmounted the filesystem, mount it again
without `--format` before continuing. Both the mount program and nanoCoder must
run inside the same container.

## Run the Demo

In the second container shell, run:

```sh
cd /work
mountpoint /tmp/cse-mnt
```

The output must say `/tmp/cse-mnt is a mountpoint`. If it does not, start the mount
program before continuing; otherwise the demo would use an ordinary directory.

```sh
python3 -m nanocoder.main \
  --task hello \
  --mock examples/hello.json \
  --workspace /tmp/cse-mnt
```

nanoCoder performs four tool calls:

1. Write `notes/todo.md`.
2. List the workspace with `ls -R .`.
3. Replace `learn nanocoder` with `learn nanocoder v1`.
4. Read the edited file.

Check the tool results in the terminal for errors, then inspect the file:

```sh
cat /tmp/cse-mnt/notes/todo.md
```

The contents should be:

```text
# TODO
- learn nanocoder v1
```

The run also creates `logs/<timestamp>/` in the workspace, containing
`events.jsonl`, `llm.log` and `session.json`. These files are stored in nanoFS
alongside `notes/todo.md`.

## Check That the File Is Saved

Unmount from the second container shell:

```sh
scripts/lab1/stop-fs.sh /tmp/cse-mnt
```

In the first container shell, mount the same image again without formatting it:

```sh
scripts/lab1/start-local.sh /tmp/cse-mnt /tmp/cse-lab1.img
```

In the second shell, check that the edited contents remain, then unmount:

```sh
cat /tmp/cse-mnt/notes/todo.md
scripts/lab1/stop-fs.sh /tmp/cse-mnt
```

This demo supplements the [required Lab1A tests](lab1.md#grading); it does not
replace them. The [submission file](lab1.md#handin) is still `nanofs/src/fs.cc`.
