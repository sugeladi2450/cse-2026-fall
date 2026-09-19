"""File tools: read_file / write_file / edit_file, relative to a workspace root.

Direct local file I/O (nanoCoder v1). The FileSystem abstraction / RemoteFS
backend described in SPEC.md is deferred to later labs.
"""

from pathlib import Path

DEFAULT_READ_LIMIT = 2000
MAX_BYTES = 50 * 1024
MAX_LINE_LENGTH = 2000


class FsTools:
    def __init__(self, workspace):
        self.workspace = Path(workspace).resolve()

    def _resolve(self, path: str) -> Path:
        # v1 is local I/O, but file tools still stay inside the workspace root.
        candidate = Path(path)
        if candidate.is_absolute():
            raise ValueError(f"path must be relative to workspace: {path!r}")
        target = (self.workspace / candidate).resolve()
        try:
            target.relative_to(self.workspace)
        except ValueError:
            raise ValueError(f"path escapes workspace: {path!r}")
        return target

    def read_file(self, path: str, offset: int = None, limit: int = None) -> str:
        """Read a workspace file as line-numbered text, windowed for size.

        `offset` is a 1-indexed start line (default 1); `limit` caps returned
        lines (default DEFAULT_READ_LIMIT). Long lines are truncated to
        MAX_LINE_LENGTH; total output is capped at MAX_BYTES. When more of the
        file remains, a continuation line states totals and the next offset.
        Line numbers are display-only and are NOT part of file content."""
        text = self._resolve(path).read_text()
        lines = text.splitlines()
        total = len(lines)
        start = max(1, offset or 1)
        limit = DEFAULT_READ_LIMIT if limit is None else limit
        end = min(total, start - 1 + limit)

        out, size, stopped_at = [], 0, end
        for n in range(start, end + 1):
            line = lines[n - 1]
            if len(line) > MAX_LINE_LENGTH:
                line = line[:MAX_LINE_LENGTH] + " ... (line truncated to 2000 chars)"
            rendered = f"{n:>6}\t{line}"
            if size + len(rendered) + 1 > MAX_BYTES:
                stopped_at = n - 1
                break
            out.append(rendered)
            size += len(rendered) + 1

        body = "\n".join(out)
        if stopped_at < total:
            body += (f"\n\n(Showing lines {start}-{stopped_at} of {total}. "
                     f"Use offset={stopped_at} to continue.)")
        return body

    def write_file(self, path: str, content: str) -> str:
        target = self._resolve(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
        return f"wrote {len(content)} chars to {path}"

    def edit_file(self, path: str, old: str, new: str) -> str:
        target = self._resolve(path)
        text = target.read_text()
        count = text.count(old)
        if count == 0:
            raise ValueError(f"substring not found in {path}: {old!r}")
        if count > 1:
            raise ValueError(f"substring not unique in {path} ({count}x): {old!r}")
        target.write_text(text.replace(old, new))
        return f"edited {path}"
