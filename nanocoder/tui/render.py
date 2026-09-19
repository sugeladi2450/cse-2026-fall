"""Rich rendering for the CSE client.

This is the only module that depends on Rich. `RichUI` implements the small
interface the driver calls (on_text_delta / on_tool_call / on_tool_result /
approve / on_turn_done) plus banner() and on_error() for the REPL shell.
"""

import json
from pathlib import Path

from rich.console import Console
from rich.panel import Panel
from rich.prompt import Confirm

_LOGO = r"""
  ____ ____  _____
 / ___/ ___|| ____|
| |   \___ \|  _|
| |___ ___) | |___
 \____|____/|_____|
"""
BANNER_TITLE = "CSE · nanoCoder"
BANNER_DESCRIPTION = (
    "This is a demo coder used for illustrating systems infrastructure powering the nowadays agents."
)


def cse_banner() -> str:
    """ASCII-art CSE logo with a short product label."""
    return _LOGO + f"\n        {BANNER_TITLE}\n" + BANNER_DESCRIPTION + "\n"


def repo_label(workspace) -> str:
    """Short repo/workspace label for the status line."""
    path = Path(workspace).expanduser()
    try:
        resolved = path.resolve()
    except OSError:
        resolved = path
    return resolved.name or str(workspace)


def boxed_banner(width: int = None) -> str:
    """ASCII logo inside a terminal box."""
    lines = cse_banner().strip("\n").splitlines()
    content_width = max(len(line) for line in lines)
    if width is not None:
        content_width = max(content_width, width - 4)
    framed = ["╭" + "─" * (content_width + 2) + "╮"]
    framed.extend(f"│ {line.ljust(content_width)} │" for line in lines)
    framed.append("╰" + "─" * (content_width + 2) + "╯")
    return "\n".join(framed)


def banner_meta(model=None, workspace=None, mode=None) -> str:
    """One-line runtime context for the pinned status row."""
    context = []
    if model:
        context.append(f"[{model}]")
    if workspace:
        context.append(f"repo: {repo_label(workspace)}")
    if mode:
        context.append(f"mode: {mode}")
    return " ".join(context)


def banner_text(model=None, workspace=None, mode=None, width: int = None, include_context: bool = True) -> str:
    """Boxed banner plus an optional one-line runtime context."""
    lines = [boxed_banner(width=width)]
    context = banner_meta(model=model, workspace=workspace, mode=mode)
    if include_context and context:
        lines.append(context)
    return "\n".join(lines)


def _summarize(name: str, args: dict) -> str:
    """Compact one-line argument summary for a tool call."""
    if name == "bash":
        return args.get("command", "")
    if "path" in args:
        return args["path"]
    return json.dumps(args, ensure_ascii=False)


class RichUI:
    def __init__(self, console: Console = None, auto_approve: bool = False):
        self.console = console or Console()
        self.auto_approve = auto_approve
        self._buffer = ""

    def banner(self):
        self.console.print(f"[bright_cyan]{boxed_banner(width=self.console.width)}[/bright_cyan]")

    def on_user_message(self, text: str):
        # echo the submitted line into the transcript (the input box clears)
        self.console.print(f"[bold cyan]›[/bold cyan] {text}")

    def on_text_delta(self, text: str):
        self._buffer += text

    def _flush(self):
        if self._buffer:
            self.console.print(self._buffer.rstrip("\n"))
            self._buffer = ""

    def on_tool_call(self, name: str, args: dict):
        self._flush()
        self.console.print(
            Panel(_summarize(name, args), title=f"[bold]{name}[/bold]",
                  border_style="yellow", expand=False)
        )

    def on_tool_result(self, name: str, observation: str):
        text = observation if len(observation) <= 800 else observation[:800] + " …"
        self.console.print(f"[dim]{text}[/dim]")

    def approve(self, name: str, args: dict) -> bool:
        if self.auto_approve:
            return True
        return Confirm.ask(f"[bold yellow]Run {name}?[/bold yellow]", default=False)

    def on_turn_done(self, final_text: str):
        if self._buffer:
            self._flush()
        elif final_text:
            self.console.print(final_text)
        self.console.print()

    def on_error(self, exc: Exception):
        self.console.print(f"[bold red]error:[/bold red] {exc}")

    def on_status(self, text: str):
        self.console.print(f"[dim]{text}[/dim]")
