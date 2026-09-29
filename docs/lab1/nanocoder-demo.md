# Running nanoCoder on nanoFS

You can use either the Lab1A local filesystem or the Lab1B remote filesystem as
nanoCoder's workspace. nanoCoder uses ordinary file operations; FUSE forwards
them to your filesystem locally in A and through RPC in B. The application code
in [`nanocoder/`](../../nanocoder/) is provided and does not need modification.

This demo uses [`examples/hello.json`](../../examples/hello.json) to specify a
sequence of tool calls instead of asking a language model to choose them. The
file operations are real; no model service, API key or additional Python packages
are needed.

## Mount Your Filesystem

Complete the implementation and pass the unit tests for the stage you want to
try. Lab1B requires your completed Lab1A implementation as well. Start the mount
using the corresponding guide:

| Stage | Unit tests | Mount instructions | Mount directory |
| --- | --- | --- | --- |
| Lab1A | [Unit Tests](lab1.md#unit-tests) | [Playing with Your Filesystem](lab1a.md#playing-with-your-filesystem) | `/tmp/cse-mnt` |
| Lab1B | [Compile and Test](lab1b.md#compile-and-test) | [Playing with Your Remote Filesystem](lab1b.md#playing-with-your-remote-filesystem) | `/tmp/cse-remote-mnt` |

Leave the mount program running, and keep the server running for Lab1B. If you
already finished the guide and unmounted, use the same disk image without
`--format`:

- **Lab1A:** run the local mount command again.
- **Lab1B:** restart the server if it has stopped, then run the remote mount
  command again, as described in [Stop and Restart](lab1b.md#stop-and-restart).

Run nanoCoder in another shell inside the **same container as the FUSE client**:
terminal 2 for the Lab1A guide, or terminal 3 for Lab1B. If the server is in a
separate container or on another computer, run nanoCoder on the client side.

## Run the Demo

In that container shell, select **one** mount directory for this session.

For Lab1A:

```sh
NANOFS_DEMO_MOUNT=/tmp/cse-mnt
```

For Lab1B:

```sh
NANOFS_DEMO_MOUNT=/tmp/cse-remote-mnt
```

From `/work`, check the mount and run the same demo for either stage:

```sh
cd /work
mountpoint "$NANOFS_DEMO_MOUNT" &&
python3 -m nanocoder.main \
  --task hello \
  --mock examples/hello.json \
  --workspace "$NANOFS_DEMO_MOUNT"
```

`mountpoint` must report that the selected directory is a mountpoint. If it does
not, the command above will not start nanoCoder; start the mount before retrying.

nanoCoder performs four tool calls:

1. Write `notes/todo.md`.
2. List the workspace with `ls -R .`.
3. Replace `learn nanocoder` with `learn nanocoder v1`.
4. Read the edited file.

Check the tool results in the terminal for errors, then inspect the file:

```sh
cat "$NANOFS_DEMO_MOUNT/notes/todo.md"
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

Use the shutdown and restart steps for your stage:

- **Lab1A:** follow the unmount and remount steps in
  [Playing with Your Filesystem](lab1a.md#playing-with-your-filesystem).
- **Lab1B:** follow [Stop and Restart](lab1b.md#stop-and-restart): unmount the
  client, stop the server cleanly, restart the server and mount the client again.

Use the same disk image and omit `--format` when restarting. Pause after
remounting, before the guide's final unmount. Return to the demo shell and check
the file. If you opened a
new shell, set `NANOFS_DEMO_MOUNT` again for the same stage first.

```sh
mountpoint "$NANOFS_DEMO_MOUNT" && cat "$NANOFS_DEMO_MOUNT/notes/todo.md"
```

The edited contents should still be present. Finish by unmounting the client;
for Lab1B, stop the server after the mount program exits, as described in its guide.

This demo supplements the required unit and stress tests; it does not replace
them. Follow the submission instructions for your stage:
[Lab1A](lab1.md#handin) or [Lab1B](lab1b.md#grading-and-handin).
