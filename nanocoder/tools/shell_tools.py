"""Shell tool: run a bash command in the workspace, with a timeout."""

import os
import signal
import subprocess
import time


class ShellTools:
    def __init__(self, workspace, timeout: float = 30.0, max_output: int = 16 * 1024):
        self.workspace = str(workspace)
        self.timeout = timeout
        self.max_output = max_output

    def bash(self, command: str, description: str = "", cancel=None) -> str:
        # description is shown in the approval prompt only; it does not affect execution.
        proc = subprocess.Popen(
            command,
            shell=True,
            cwd=self.workspace,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,  # own process group, so we can signal a whole subtree
        )
        start = time.monotonic()
        while True:
            try:
                stdout, stderr = proc.communicate(timeout=0.1)
                break
            except subprocess.TimeoutExpired:
                if cancel is not None and cancel.cancelled:
                    out, err = self._terminate(proc)
                    return self._format("command cancelled by user", None, out, err)
                if time.monotonic() - start > self.timeout:
                    self._terminate(proc)
                    return f"command timed out after {self.timeout}s"

        return self._format(None, proc.returncode, stdout, stderr)

    def _format(self, prefix, returncode, stdout, stderr) -> str:
        """Build an observation string: optional `prefix` line, optional
        `exit code:` line (when `returncode` is not None), then non-empty
        `stdout:`/`stderr:` sections. Truncates to `self.max_output` chars."""
        parts = []
        if prefix is not None:
            parts.append(prefix)
        if returncode is not None:
            parts.append(f"exit code: {returncode}")
        if stdout:
            parts.append(f"stdout:\n{stdout}")
        if stderr:
            parts.append(f"stderr:\n{stderr}")
        result = "\n".join(parts)
        if len(result) > self.max_output:
            dropped = len(result) - self.max_output
            result = result[:self.max_output] + f"\n…[truncated {dropped} chars]…"
        return result

    def _terminate(self, proc) -> tuple[str, str]:
        """Stop a running child and any of its own children (terminate, then
        kill if stubborn), reap it, and return whatever (stdout, stderr) it
        had produced. Signals the whole process group: a shell running a
        compound command (e.g. `echo x; sleep 10`) forks real children, and
        signaling only the shell pid would leave them running as orphans."""
        self._signal_group(proc, signal.SIGTERM)
        try:
            return proc.communicate(timeout=1.0)
        except subprocess.TimeoutExpired:
            self._signal_group(proc, signal.SIGKILL)
            return proc.communicate()

    @staticmethod
    def _signal_group(proc, sig) -> None:
        """Send `sig` to proc's whole process group; ignore if it's already gone."""
        try:
            os.killpg(os.getpgid(proc.pid), sig)
        except ProcessLookupError:
            pass
