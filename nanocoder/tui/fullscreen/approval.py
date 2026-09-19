from dataclasses import dataclass, field
import threading

from ...permission import session_scope


@dataclass
class ApprovalState:
    event: threading.Event | None = None
    result: bool | str = False
    name: str = ""
    args: dict = field(default_factory=dict)
    selected: int = 0
    feedback_mode: bool = False
    feedback_draft: str = ""

    @property
    def awaiting(self) -> bool:
        return self.event is not None

    @property
    def collecting_feedback(self) -> bool:
        return self.awaiting and self.feedback_mode

    def reset_for(self, name, args):
        self.event = threading.Event()
        self.result = False
        self.name = name
        self.args = args
        self.selected = 0
        self.feedback_mode = False
        self.feedback_draft = ""

    def clear(self):
        self.event = None
        self.result = False
        self.name = ""
        self.args = {}
        self.selected = 0
        self.feedback_mode = False
        self.feedback_draft = ""


TOOL_HEADERS = {
    "bash": "Bash command",
    "edit_file": "Edit file",
    "write_file": "Write file",
    "read_file": "Read file",
}


def approval_header(name):
    return TOOL_HEADERS.get(name, f"Tool: {name}")


def approval_body(name, args):
    if name == "bash":
        return [args.get("command", "")]
    path = args.get("path")
    if path:
        return [path]
    return [f"{key}: {value}" for key, value in args.items()]


def approval_notice(name):
    if name == "bash":
        return "This command requires approval"
    return "This action requires approval"


def _wrap(text, width):
    lines = []
    for segment in text.split("\n"):
        while len(segment) > width:
            lines.append(segment[:width])
            segment = segment[width:]
        lines.append(segment)
    return lines


def _card_lines(state, width):
    indent = "  "
    body_width = max(width - len(indent), 1)
    lines = [("class:approval-header", approval_header(state.name)), ("class:muted", "")]
    for raw in approval_body(state.name, state.args):
        for wrapped in _wrap(raw, body_width):
            lines.append(("class:transcript", indent + wrapped))
    description = state.args.get("description")
    if state.name == "bash" and description:
        for wrapped in _wrap(description, body_width):
            lines.append(("class:muted", indent + wrapped))
    lines.append(("class:muted", ""))
    lines.append(("class:muted", approval_notice(state.name)))
    return lines


def _menu_lines(state):
    no_option = "3. No"
    if state.feedback_mode:
        no_option = f"3. No: {state.feedback_draft}"
    options = [
        "1. Yes",
        f"2. Yes, allow all {session_scope(state.name)} during this session (alt+m)",
        no_option,
    ]
    lines = [("class:muted", "")]
    for index, option in enumerate(options):
        marker = "› " if index == state.selected else "  "
        style = "class:accent" if index == state.selected else "class:transcript"
        lines.append((style, f"{marker}{option}"))
    lines.append(("class:muted", ""))
    if state.feedback_mode:
        lines.append(("class:muted", "Enter to send · Esc to cancel"))
    else:
        lines.append(("class:muted", "Esc to cancel · Tab to add additional instructions"))
    return lines


def approval_lines(state, width):
    """Return (style, text) lines: the tool card followed by the choice menu."""
    return _card_lines(state, width) + _menu_lines(state)
