import threading

from .constants import MAX_RENDER_LINES
from ..render import _summarize, banner_meta, banner_text


class TranscriptModel:
    """Plain in-memory transcript for the full-screen renderer."""

    def __init__(self):
        self._lines = []
        self._roles = []
        self.pinned_status = ""
        self._streaming = False
        self._lock = threading.RLock()

    def _append(self, role: str, line: str):
        self._lines.append(line)
        self._roles.append(role)

    def _extend(self, role: str, lines):
        for line in lines:
            self._append(role, line)

    def banner(self, model=None, workspace=None, mode=None):
        with self._lock:
            self.pinned_status = banner_meta(model=model, workspace=workspace, mode=mode)
            self._extend("banner", banner_text(include_context=False).splitlines())
            self._append("blank", "")
            self._streaming = False

    def clear(self):
        """Drop the visible transcript back to a fresh banner (used by /clear).

        Keeps the pinned status line; re-renders the banner so the screen looks
        like a new session rather than going blank."""
        with self._lock:
            self._lines = []
            self._roles = []
            self._streaming = False
            self._extend("banner", banner_text(include_context=False).splitlines())
            self._append("blank", "")

    def on_user_message(self, text: str):
        with self._lock:
            self._append("user", f"› {text}")
            self._streaming = False

    def on_text_delta(self, text: str):
        with self._lock:
            if not self._streaming:
                self._append("assistant", "")
                self._streaming = True
            self._lines[-1] += text

    def on_tool_call(self, name: str, args: dict):
        with self._lock:
            self._append("tool", f"tool: {name}")
            self._append("tool-detail", f"  {_summarize(name, args)}")
            self._streaming = False

    def on_tool_result(self, name: str, observation: str):
        text = observation if len(observation) <= 800 else observation[:800] + " ..."
        with self._lock:
            self._extend(f"tool-result:{name}", text.splitlines() or [""])
            self._streaming = False

    def on_turn_done(self, final_text: str):
        with self._lock:
            if not self._streaming and final_text:
                self._append("assistant", final_text)
            self._append("blank", "")
            self._streaming = False

    def on_status(self, text: str):
        with self._lock:
            if self._lines and self._lines[-1] == "":
                self._lines[-1] = text
                self._roles[-1] = "status"
                self._append("blank", "")
            else:
                self._append("status", text)
            self._streaming = False

    def on_error(self, exc: Exception):
        with self._lock:
            self._append("error", f"error: {exc}")
            self._streaming = False

    def text(self) -> str:
        with self._lock:
            return "\n".join(self._lines).rstrip()

    def tail_text(self, max_lines: int = MAX_RENDER_LINES) -> str:
        with self._lock:
            return "\n".join(self._lines[-max_lines:]).rstrip()

    def tail_items(self, max_lines: int = MAX_RENDER_LINES) -> list[tuple[str, str]]:
        # prompt_toolkit renders a literal tab as "^I"; expand tabs to spaces so
        # tab-separated tool output (e.g. read_file's "{n:>6}\t{line}") displays
        # aligned. This is the single chokepoint feeding both rendering and
        # selection, so displayed and copied text stay in sync.
        with self._lock:
            items = []
            for role, line in zip(self._roles, self._lines):
                parts = line.expandtabs().splitlines()
                if parts:
                    items.extend((role, part) for part in parts)
                else:
                    items.append((role, ""))
            return items[-max_lines:]
