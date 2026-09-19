import asyncio
import re
import time

from prompt_toolkit.application import Application
from prompt_toolkit.mouse_events import MouseButton, MouseEventType
from prompt_toolkit.utils import get_cwidth

from .approval import ApprovalState, approval_lines
from .constants import (
    APPROVAL_BOX_HEIGHT,
    BOTTOM_AREA_HEIGHT,
    MAX_RENDER_LINES,
    MIN_REFRESH_SECONDS,
    MOUSE_SUPPORT,
    THINKING_WORDS,
)
from .model import TranscriptModel
from .selection import copy_selection
from ..render import BANNER_DESCRIPTION, BANNER_TITLE
from ...agent.cancel import CancelToken
from ...permission import PermissionHarness


class FixedBottomUI:
    """UI object consumed by driver.drive inside the full-screen app."""

    def __init__(
        self,
        transcript: TranscriptModel,
        auto_approve: bool = False,
        clock=None,
        clipboard_writer=None,
    ):
        self.transcript = transcript
        self.permissions = PermissionHarness(auto_approve_all=auto_approve)
        self._clock = clock or time.monotonic
        self._clipboard_writer = clipboard_writer
        self.app = None
        self.focus_target = None
        self.loop = None
        self._approval = ApprovalState()
        self._approval_async_event = None
        self._cancel_hook = None
        self._last_refresh = 0.0
        self.scroll_offset = 0
        self.visible_lines = MAX_RENDER_LINES
        self.visible_columns = 80
        self._turn_started_at = None
        self._waiting_for_response = False
        self.exit_requested = False
        self.turn_running = False
        self.cancel_token = None
        self.selection_mode = False
        self._selecting = False
        self._selection_anchor = None
        self._selection_cursor = None
        self.selected_text = ""

    @property
    def awaiting_approval(self) -> bool:
        return self._approval.awaiting

    @property
    def _approval_event(self):
        return self._approval.event

    @_approval_event.setter
    def _approval_event(self, event):
        self._approval.event = event

    def attach(self, app: Application, focus_target=None, loop=None):
        self.app = app
        self.focus_target = focus_target
        self.loop = loop
        self._update_visible_lines()

    def display_text(self) -> str:
        return "\n".join(self._display_lines())

    def _approval_lines(self):
        return approval_lines(self._approval, self.visible_columns)

    def approval_text(self) -> str:
        if not self.awaiting_approval:
            return ""
        return "\n".join(text for _style, text in self._approval_lines())

    def approval_height(self) -> int:
        if not self.awaiting_approval:
            return 0
        return max(APPROVAL_BOX_HEIGHT, len(self._approval_lines()))

    def _display_lines(self) -> list[str]:
        return [line for _role, line in self._display_items()]

    def _display_items(self) -> list[tuple[str, str]]:
        items = self.transcript.tail_items(MAX_RENDER_LINES)
        if self._waiting_for_response and self._turn_started_at is not None:
            items.append(("status", self._wait_status()))
        items = self._visual_items(items)
        budget = max(1, min(self.visible_lines, MAX_RENDER_LINES))
        max_offset = max(0, len(items) - budget)
        self.scroll_offset = min(self.scroll_offset, max_offset)
        end = len(items) - self.scroll_offset
        start = self._fit_start(items, end, budget)
        return items[start:end]

    def _fit_start(self, items: list[tuple[str, str]], end: int, budget: int) -> int:
        """First index so items[start:end] fill at most `budget` terminal rows.

        The transcript window wraps (wrap_lines=True), so a wide line spans more
        than one row; budgeting by logical lines would let the wrapped content
        overflow and the renderer would clip the newest lines at the bottom.
        Walk newest-first, keeping whole lines until the next would overflow;
        the last line is always kept even if it alone exceeds the budget."""
        rows = 0
        start = end
        for index in range(end - 1, -1, -1):
            span = self._row_span(items[index])
            if index < end - 1 and rows + span > budget:
                break
            rows += span
            start = index
        return start

    def _row_span(self, item: tuple[str, str]) -> int:
        """Terminal rows a visual item occupies once wrapped at the current
        width. Simulates prompt_toolkit's own wrap rule (`if x + char_width >
        width: wrap`) character by character so a double-width (CJK) glyph that
        cannot straddle the boundary counts its extra row — a plain
        ceil(width/columns) underestimates those and clips the newest line.
        Assistant lines carry the 2-column marker/indent that display_fragments
        prepends."""
        role, line = item
        columns = max(1, self.visible_columns)
        x = 2 if self._is_assistant_role(role) else 0
        rows = 1
        for char in line:
            char_width = max(0, get_cwidth(char))
            if x + char_width > columns:
                rows += 1
                x = 0
            x += char_width
        return rows

    def _visual_items(self, items: list[tuple[str, str]]) -> list[tuple[str, str]]:
        visual = []
        assistant_lines = []

        def flush_assistant():
            if not assistant_lines:
                return
            first_line = True
            for role, line in self._markdown_assistant_items(assistant_lines):
                visual.append((self._assistant_visual_role(role, first_line), line))
                first_line = False
            assistant_lines.clear()

        for index, (role, line) in enumerate(items):
            if role == "assistant":
                assistant_lines.append(line)
                continue
            flush_assistant()
            if self._is_consumed_bash_result_line(items, index):
                continue
            if role == "tool-result:bash":
                visual.extend(self._bash_result_items(items, index))
                continue
            visual.append((role, line))
            if role == "user" and self._next_response_role(items, index) in {"assistant", "status"}:
                visual.append(("gap", ""))
        flush_assistant()
        return visual

    def _bash_result_items(self, items: list[tuple[str, str]], index: int) -> list[tuple[str, str]]:
        lines = []
        for role, line in items[index:]:
            if role != "tool-result:bash":
                break
            lines.append(line)
        display = self._format_bash_result(lines)
        return [("tool-result:bash", line) for line in display]

    def _is_consumed_bash_result_line(self, items: list[tuple[str, str]], index: int) -> bool:
        return index > 0 and items[index][0] == "tool-result:bash" and items[index - 1][0] == "tool-result:bash"

    def _format_bash_result(self, lines: list[str]) -> list[str]:
        if not lines or not lines[0].startswith("exit code: 0"):
            return lines
        output = []
        section = None
        for line in lines[1:]:
            if line == "stdout:":
                section = "stdout"
                continue
            if line == "stderr:":
                section = "stderr"
                continue
            if section == "stdout":
                output.append(line)
        return output or ["(no output)"]

    def _assistant_visual_role(self, role: str, first_line: bool) -> str:
        if first_line:
            return f"{role}-start"
        return role

    def _markdown_assistant_items(self, lines: list[str]) -> list[tuple[str, str]]:
        items = []
        index = 0
        in_code = False
        while index < len(lines):
            line = lines[index]
            stripped = line.strip()
            if not stripped:
                index += 1
                continue
            if stripped.startswith("```"):
                in_code = not in_code
                index += 1
                continue
            if in_code:
                items.append(("assistant-code", line))
                index += 1
                continue
            if self._starts_markdown_table(lines, index):
                table_lines, index = self._consume_markdown_table(lines, index)
                items.extend(table_lines)
                continue
            heading = re.match(r"^(#{1,6})\s+(.+)$", stripped)
            if heading:
                items.append(("assistant-heading", heading.group(2)))
                index += 1
                continue
            if re.match(r"^([-*+]|\d+[.])\s+", stripped):
                items.append(("assistant-list", stripped))
                index += 1
                continue
            items.append(("assistant", line))
            index += 1
        return items

    def _starts_markdown_table(self, lines: list[str], index: int) -> bool:
        return (
            index + 1 < len(lines)
            and self._markdown_table_cells(lines[index]) is not None
            and self._is_markdown_table_separator(lines[index + 1])
        )

    def _consume_markdown_table(self, lines: list[str], index: int):
        rows = [self._markdown_table_cells(lines[index])]
        index += 2
        while index < len(lines):
            cells = self._markdown_table_cells(lines[index])
            if cells is None:
                break
            rows.append(cells)
            index += 1
        columns = max(len(row) for row in rows)
        rows = [row + [""] * (columns - len(row)) for row in rows]
        widths = [max(len(row[column]) for row in rows) for column in range(columns)]
        top = "┌" + "┬".join("─" * (width + 2) for width in widths) + "┐"
        middle = "├" + "┼".join("─" * (width + 2) for width in widths) + "┤"
        bottom = "└" + "┴".join("─" * (width + 2) for width in widths) + "┘"
        table = [("assistant-table", top)]
        for row_index, row in enumerate(rows):
            padded = [cell.ljust(widths[column]) for column, cell in enumerate(row)]
            role = "assistant-table-header" if row_index == 0 else "assistant-table"
            table.append((role, "│ " + " │ ".join(padded) + " │"))
            if row_index == 0:
                table.append(("assistant-table", middle))
        table.append(("assistant-table", bottom))
        return table, index

    def _markdown_table_cells(self, line: str) -> list[str] | None:
        stripped = line.strip()
        if not stripped.startswith("|") or not stripped.endswith("|"):
            return None
        cells = [self._strip_inline_code(cell.strip()) for cell in stripped.strip("|").split("|")]
        if len(cells) < 2:
            return None
        return cells

    def _is_markdown_table_separator(self, line: str) -> bool:
        cells = self._markdown_table_cells(line)
        if cells is None:
            return False
        return all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells)

    def _strip_inline_code(self, text: str) -> str:
        return re.sub(r"`([^`]+)`", r"\1", text)

    def _next_response_role(self, items: list[tuple[str, str]], index: int) -> str | None:
        for role, line in items[index + 1:]:
            if role == "assistant" and not line:
                continue
            if not line and role in {"blank", "gap"}:
                continue
            return role
        return None

    def approval_fragments(self):
        if not self.awaiting_approval:
            return []
        fragments = []
        for style, line in self._approval_lines():
            fragments.append((style, line))
            fragments.append(("", "\n"))
        return fragments[:-1]

    def _elapsed(self) -> float:
        if self._turn_started_at is None:
            return 0.0
        return max(0.0, self._clock() - self._turn_started_at)

    def _wait_status(self) -> str:
        elapsed = self._elapsed()
        word = THINKING_WORDS[int(elapsed // 2) % len(THINKING_WORDS)]
        return f"{word}... {elapsed:.1f}s"

    def display_fragments(self):
        items = self._display_items()
        if not items:
            return []
        fragments = []
        for y, (role, line) in enumerate(items):
            line = self._expand_banner_line(line)
            if role == "user":
                fragments.extend(self._user_input_fragments(y, line))
                fragments.append(("", "\n"))
                continue
            if self._is_assistant_role(role) and line:
                fragments.extend(self._assistant_fragments(y, line, role))
                fragments.append(("", "\n"))
                continue
            if BANNER_TITLE in line:
                fragments.extend(self._banner_text_fragments(line, BANNER_TITLE, "class:banner-title"))
                fragments.append(("", "\n"))
                continue
            if BANNER_DESCRIPTION in line:
                fragments.extend(self._banner_text_fragments(line, BANNER_DESCRIPTION, "class:muted"))
                fragments.append(("", "\n"))
                continue
            style = "class:transcript"
            lowered = line.lower()
            if line.startswith(("╭", "│", "╰")):
                style = "class:banner-border"
            elif line.startswith("[") and "repo:" in line:
                style = "class:banner-meta"
            elif "cse" in lowered or "nanocoder" in lowered or line.startswith("tool:"):
                style = "class:accent"
            if line.startswith("error:") or "reject" in lowered:
                style = "class:warning"
            elif line.startswith("  ") or line.startswith("completed in") or any(
                line.startswith(f"{word}...") for word in THINKING_WORDS
            ):
                style = "class:muted"
            fragments.extend(self._line_fragments(y, line, style))
            fragments.append(("", "\n"))
        return fragments[:-1]

    def _user_input_fragments(self, y: int, line: str):
        width = max(self.visible_columns, len(line), 1)
        padded = line.ljust(width)
        if self._selected_span_for_line(y, padded) is not None:
            return self._line_fragments(y, padded, "class:user-input-row")
        if not padded.startswith("›"):
            return [("class:user-input-row", padded, self.handle_transcript_mouse)]
        return [
            ("class:user-input-prompt", "›", self.handle_transcript_mouse),
            ("class:user-input-row", padded[1:], self.handle_transcript_mouse),
        ]

    def _is_assistant_role(self, role: str) -> bool:
        return role == "assistant" or role.startswith("assistant-")

    def _assistant_fragments(self, y: int, line: str, role: str):
        first_line = role.endswith("-start")
        base_role = role[:-6] if first_line else role
        style = self._assistant_style(base_role)
        if self._selected_span_for_line(y, line) is not None:
            return self._line_fragments(y, line, style)
        prefix = "" if base_role in {"assistant-table", "assistant-table-header"} else (" " if first_line else "  ")
        text_fragments = self._inline_markdown_fragments(style, f"{prefix}{line}")
        if first_line:
            return [("class:assistant-marker", "●", self.handle_transcript_mouse), *text_fragments]
        return text_fragments

    def _assistant_style(self, role: str) -> str:
        styles = {
            "assistant-code": "class:markdown-code",
            "assistant-heading": "class:markdown-heading",
            "assistant-list": "class:markdown-list",
            "assistant-table": "class:markdown-table",
            "assistant-table-header": "class:markdown-table-header",
        }
        return styles.get(role, "class:assistant-text")

    def _inline_markdown_fragments(self, style: str, text: str):
        fragments = []
        cursor = 0
        for match in re.finditer(r"`([^`]+)`", text):
            if match.start() > cursor:
                fragments.append((style, text[cursor:match.start()], self.handle_transcript_mouse))
            fragments.append(("class:inline-code", match.group(1), self.handle_transcript_mouse))
            cursor = match.end()
        if cursor < len(text):
            fragments.append((style, text[cursor:], self.handle_transcript_mouse))
        return fragments

    def _banner_text_fragments(self, line: str, text: str, style: str):
        start = line.index(text)
        end = start + len(text)
        fragments = []
        if start > 0:
            fragments.append(("class:banner-border", line[:start], self.handle_transcript_mouse))
        fragments.append((style, text, self.handle_transcript_mouse))
        if end < len(line):
            fragments.append(("class:banner-border", line[end:], self.handle_transcript_mouse))
        return fragments

    def pinned_status_fragments(self):
        status = self.transcript.pinned_status
        if not status:
            return [("class:banner-meta", "")]
        fragments = []
        if status.startswith("["):
            end = status.find("]")
            if end != -1:
                fragments.append(("class:banner-model", status[: end + 1]))
                status = status[end + 1 :]
        if status.startswith(" "):
            fragments.append(("class:banner-meta", " "))
            status = status[1:]
        if status.startswith("repo:"):
            mode_start = status.find(" mode:")
            if mode_start == -1:
                fragments.append(("class:banner-repo", status))
                return fragments
            fragments.append(("class:banner-repo", status[:mode_start]))
            fragments.append(("class:banner-meta", status[mode_start:]))
            return fragments
        if status:
            fragments.append(("class:banner-meta", status))
        return fragments

    def _expand_banner_line(self, line: str) -> str:
        width = max(self.visible_columns, 4)
        if line.startswith("╭") and line.endswith("╮"):
            return "╭" + "─" * (width - 2) + "╮"
        if line.startswith("╰") and line.endswith("╯"):
            return "╰" + "─" * (width - 2) + "╯"
        if line.startswith("│") and line.endswith("│"):
            content = line[2:-2].rstrip() if len(line) >= 4 else ""
            return "│ " + content.ljust(width - 4) + " │"
        return line

    def _line_fragments(self, y: int, line: str, style: str):
        span = self._selected_span_for_line(y, line)
        if span is None:
            return [(style, line, self.handle_transcript_mouse)]
        start, end = span
        fragments = []
        if start > 0:
            fragments.append((style, line[:start], self.handle_transcript_mouse))
        fragments.append((f"{style} class:selection", line[start:end], self.handle_transcript_mouse))
        if end < len(line):
            fragments.append((style, line[end:], self.handle_transcript_mouse))
        return fragments

    def _selected_span_for_line(self, y: int, line: str):
        selection = self._normalized_selection()
        if selection is None:
            return None
        (start_y, start_x), (end_y, end_x) = selection
        if y < start_y or y > end_y:
            return None
        if start_y == end_y:
            start = min(start_x, len(line))
            end = min(end_x, len(line))
        elif y == start_y:
            start = min(start_x, len(line))
            end = len(line)
        elif y == end_y:
            start = 0
            end = min(end_x, len(line))
        else:
            start = 0
            end = len(line)
        if end <= start:
            return None
        return start, end

    def _normalized_selection(self):
        if self._selection_anchor is None or self._selection_cursor is None:
            return None
        anchor_y, anchor_x = self._selection_anchor
        cursor_y, cursor_x = self._selection_cursor
        anchor = (anchor_y, anchor_x)
        cursor_end = (cursor_y, cursor_x + 1)
        if anchor <= (cursor_y, cursor_x):
            return anchor, cursor_end
        return (cursor_y, cursor_x), (anchor_y, anchor_x + 1)

    def mouse_scroll(self, mouse_event):
        if mouse_event.event_type == MouseEventType.SCROLL_UP:
            self.scroll_up(3)
        elif mouse_event.event_type == MouseEventType.SCROLL_DOWN:
            self.scroll_down(3)

    def handle_transcript_mouse(self, mouse_event):
        if mouse_event.event_type in {MouseEventType.SCROLL_UP, MouseEventType.SCROLL_DOWN}:
            self.mouse_scroll(mouse_event)
            return None
        if mouse_event.button != MouseButton.LEFT:
            return None

        point = self._clamp_selection_point(mouse_event.position.y, mouse_event.position.x)
        if mouse_event.event_type == MouseEventType.MOUSE_DOWN:
            self._selecting = True
            self._selection_anchor = point
            self._selection_cursor = point
            self.selected_text = ""
            self._refresh(force=True)
        elif mouse_event.event_type == MouseEventType.MOUSE_MOVE and self._selecting:
            self._selection_cursor = point
            self._refresh(force=True)
        elif mouse_event.event_type == MouseEventType.MOUSE_UP and self._selecting:
            self._selection_cursor = point
            self._selecting = False
            self.selected_text = self._selection_text()
            self._copy_selection(self.selected_text)
            self._refresh(force=True)
        return None

    def _clamp_selection_point(self, y: int, x: int):
        lines = self._display_lines()
        if not lines:
            return 0, 0
        y = max(0, min(y, len(lines) - 1))
        line = lines[y]
        if line:
            x = max(0, min(x, len(line) - 1))
        else:
            x = 0
        return y, x

    def _selection_text(self) -> str:
        selection = self._normalized_selection()
        if selection is None:
            return ""
        lines = self._display_lines()
        (start_y, start_x), (end_y, end_x) = selection
        parts = []
        for y in range(start_y, min(end_y, len(lines) - 1) + 1):
            line = lines[y]
            if start_y == end_y:
                parts.append(line[start_x:min(end_x, len(line))])
            elif y == start_y:
                parts.append(line[start_x:])
            elif y == end_y:
                parts.append(line[:min(end_x, len(line))])
            else:
                parts.append(line)
        return "\n".join(parts)

    def _copy_selection(self, text: str):
        copy_selection(text, app=self.app, clipboard_writer=self._clipboard_writer)

    def _refresh_in_loop(self, keep_focus: bool):
        if self.app is not None:
            self._update_visible_lines()
            if keep_focus and self.focus_target is not None:
                self.app.layout.focus(self.focus_target)
            self.app.invalidate()

    def _refresh(self, *, force: bool = False, keep_focus: bool = False):
        if self.app is None:
            return
        now = time.monotonic()
        if not force and now - self._last_refresh < MIN_REFRESH_SECONDS:
            return
        self._last_refresh = now
        # Agent turns run in a worker thread; invalidate prompt_toolkit on its loop.
        if self.loop is not None:
            self.loop.call_soon_threadsafe(self._refresh_in_loop, keep_focus)
        else:
            self._refresh_in_loop(keep_focus)

    def scroll_up(self, lines: int = 3):
        self.clear_selection()
        self.scroll_offset += lines
        self._refresh(force=True)

    def scroll_down(self, lines: int = 3):
        self.clear_selection()
        self.scroll_offset = max(0, self.scroll_offset - lines)
        self._refresh(force=True)

    def clear_selection(self):
        self._selecting = False
        self._selection_anchor = None
        self._selection_cursor = None

    def mouse_capture_enabled(self) -> bool:
        return MOUSE_SUPPORT and not self.selection_mode

    def toggle_selection_mode(self):
        self.selection_mode = not self.selection_mode
        self._refresh(force=True, keep_focus=True)

    def on_turn_start(self):
        self._turn_started_at = self._clock()
        self._waiting_for_response = True
        self._refresh(force=True, keep_focus=True)

    def begin_turn(self) -> "CancelToken":
        """Start a new turn: mint a fresh cancel token and mark the turn live.
        Returned token is the legacy CancelToken path alongside bind_cancel's
        bus hook (see request_cancel)."""
        self.cancel_token = CancelToken()
        self.turn_running = True
        return self.cancel_token

    def can_cancel(self) -> bool:
        """True when Esc should cancel the turn (running, not mid-approval)."""
        return self.turn_running and not self.awaiting_approval

    def bind_cancel(self, hook):
        """Register a live cancel hook (e.g. bus.cancel_turn) that request_cancel
        fires in addition to the legacy CancelToken path."""
        self._cancel_hook = hook

    def request_cancel(self):
        """Signal the running turn to stop. If we're blocked on an approval,
        also reject it so the worker thread leaves approve() and hits the
        cancel check. When a live cancel hook is bound (bus-driven turn), fire
        it too so the running coroutine's in-flight requests fail promptly."""
        if self.cancel_token is not None:
            self.cancel_token.cancel()
        if self.awaiting_approval:
            self.resolve_approval(False)
        if self._cancel_hook is not None:
            self._cancel_hook()

    def on_turn_cancelled(self, note: str):
        self.clear_selection()
        self._waiting_for_response = False
        self._turn_started_at = None
        self.turn_running = False
        self.transcript.on_status(f"⨯ {note}")
        self.scroll_offset = 0
        self._refresh(force=True, keep_focus=True)

    def refresh_wait_status(self):
        if self._waiting_for_response:
            self._refresh(force=True, keep_focus=True)

    def on_user_message(self, text: str):
        self.clear_selection()
        self.transcript.on_user_message(text)
        self.scroll_offset = 0
        self._refresh(force=True, keep_focus=True)

    def clear_transcript(self):
        """Wipe the visible transcript back to a fresh banner (/clear)."""
        self.clear_selection()
        self.transcript.clear()
        self.scroll_offset = 0
        self._refresh(force=True, keep_focus=True)

    def on_text_delta(self, text: str):
        self.clear_selection()
        self._waiting_for_response = False
        self.transcript.on_text_delta(text)
        if self.scroll_offset == 0:
            self._update_visible_lines()
        self._refresh()

    def on_tool_call(self, name: str, args: dict):
        self.clear_selection()
        self._waiting_for_response = False
        self.transcript.on_tool_call(name, args)
        self.scroll_offset = 0
        self._refresh(force=True)

    def on_tool_result(self, name: str, observation: str):
        self.clear_selection()
        self._waiting_for_response = False
        self.transcript.on_tool_result(name, observation)
        self.scroll_offset = 0
        self._refresh(force=True)

    def on_turn_done(self, final_text: str):
        self.clear_selection()
        elapsed = self._elapsed()
        self._waiting_for_response = False
        self.transcript.on_turn_done(final_text)
        if self._turn_started_at is not None:
            self.transcript.on_status(f"completed in {elapsed:.1f}s")
            self._turn_started_at = None
        self.turn_running = False
        self.scroll_offset = 0
        self._refresh(force=True, keep_focus=True)

    def on_status(self, text: str):
        self.transcript.on_status(text)
        self.scroll_offset = 0
        self._refresh(force=True, keep_focus=True)

    def on_error(self, exc: Exception):
        self.clear_selection()
        self._waiting_for_response = False
        self._turn_started_at = None
        self.transcript.on_error(exc)
        self.turn_running = False
        self.scroll_offset = 0
        self._refresh(force=True, keep_focus=True)

    def approve(self, name: str, args: dict):
        if self.permissions.is_session_allowed(name):
            return True
        # drive() is running off the UI loop, so this wait cannot freeze rendering.
        self._approval.reset_for(name, args)
        self._refresh(force=True, keep_focus=True)
        self._approval.event.wait()
        result = self._approval.result
        self._approval.clear()
        self._refresh(force=True, keep_focus=True)
        return result

    async def approve_async(self, name, args):
        """Loop-native approval for the bus render responder: reuse the approval
        menu/feedback/allow-session state but await an asyncio.Event set by
        resolve_approval instead of blocking a worker thread, so prompt_toolkit
        keeps handling keypresses while we wait. Returns True / False / a
        feedback string, matching approve()."""
        if self.permissions.is_session_allowed(name):
            return True
        self._approval.reset_for(name, args)
        self._approval_async_event = asyncio.Event()
        self._refresh(force=True, keep_focus=True)
        await self._approval_async_event.wait()
        result = self._approval.result
        self._approval.clear()
        self._approval_async_event = None
        self._refresh(force=True, keep_focus=True)
        return result

    def move_approval_selection(self, delta: int):
        if not self.awaiting_approval or self._approval.feedback_mode:
            return
        self._approval.selected = (self._approval.selected + delta) % 3
        self._refresh(force=True, keep_focus=True)

    @property
    def collecting_approval_feedback(self) -> bool:
        return self._approval.collecting_feedback

    def enter_approval_feedback_mode(self):
        if not self.awaiting_approval:
            return
        self._approval.feedback_mode = True
        self._approval.selected = 2
        self._approval.feedback_draft = ""
        self._refresh(force=True, keep_focus=True)

    def cancel_approval_feedback(self):
        if not self.collecting_approval_feedback:
            return
        self._approval.feedback_mode = False
        self._approval.feedback_draft = ""
        self._refresh(force=True, keep_focus=True)

    def update_approval_feedback_draft(self, feedback: str):
        if not self.collecting_approval_feedback:
            return
        self._approval.feedback_draft = feedback.strip()
        self._refresh(force=True, keep_focus=True)

    def append_approval_feedback(self, text: str) -> str:
        if not self.collecting_approval_feedback:
            return text
        self._approval.feedback_draft += text
        self._refresh(force=True, keep_focus=True)
        return ""

    def delete_approval_feedback_char(self) -> str:
        if not self.collecting_approval_feedback:
            return ""
        self._approval.feedback_draft = self._approval.feedback_draft[:-1]
        self._refresh(force=True, keep_focus=True)
        return ""

    def resolve_approval_feedback(self, feedback: str):
        if not self.awaiting_approval:
            return
        feedback = (feedback or self._approval.feedback_draft).strip()
        self._approval.result = feedback or False
        self._approval.event.set()
        if self._approval_async_event is not None:
            self._approval_async_event.set()
        self._refresh(force=True, keep_focus=True)

    def resolve_selected_approval(self):
        if self._approval.selected == 0:
            self.resolve_approval(True)
        elif self._approval.selected == 1:
            self.resolve_approval(True, allow_session=True)
        else:
            self.resolve_approval(False)

    def resolve_approval(self, decision: bool, allow_session: bool = False):
        if not self.awaiting_approval:
            return
        if decision and allow_session:
            self.permissions.allow_session(self._approval.name)
        self._approval.result = decision
        self._approval.event.set()
        if self._approval_async_event is not None:
            self._approval_async_event.set()
        self._refresh(force=True, keep_focus=True)

    def request_exit(self):
        self.exit_requested = True
        if self.awaiting_approval:
            self.resolve_approval(False)

    def _update_visible_lines(self):
        if self.app is None:
            return
        try:
            size = self.app.output.get_size()
            self.visible_lines = max(1, size.rows - BOTTOM_AREA_HEIGHT - self.approval_height())
            self.visible_columns = max(4, size.columns)
        except Exception:
            self.visible_lines = MAX_RENDER_LINES
            self.visible_columns = 80
