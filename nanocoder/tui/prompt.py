"""A claude-code-style bordered input box, built on prompt_toolkit.

Renders a rounded box around a single-line editor that redraws as you type.
Enter submits (and the line is stored in history; ↑/↓ recall earlier inputs);
Ctrl-C / Ctrl-D return None (treated as EOF by the REPL). Falls back to plain
input() when stdin is not a TTY (e.g. piped), so the client still works in
non-interactive shells.
"""

import sys

from prompt_toolkit.application import Application, get_app
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.layout import Layout
from prompt_toolkit.layout.containers import HSplit, VSplit, Window
from prompt_toolkit.styles import Style
from prompt_toolkit.widgets import TextArea

from .history import InputHistory


def _hline(left: str, right: str):
    return VSplit(
        [Window(width=1, char=left), Window(char="─"), Window(width=1, char=right)],
        height=1,
        style="class:border",
    )


def make_reader(message: str = "› ", border: str = "ansiwhite"):
    """Return a `read_line() -> str | None` callable rendering a bordered box."""
    if not sys.stdin.isatty():
        return lambda: _plain_input(message)

    history = InputHistory()

    def _accept(buff):
        history.add(buff.text)
        get_app().exit(result=buff.text)
        return True  # keep text; we reset it before the next read

    editor = TextArea(
        prompt=message,
        multiline=False,
        wrap_lines=False,
        accept_handler=_accept,
        style="class:input",
    )

    def _show(text):
        if text is not None:
            editor.text = text
            editor.buffer.cursor_position = len(text)

    kb = KeyBindings()

    @kb.add("up")
    def _(event):
        _show(history.older())

    @kb.add("down")
    def _(event):
        _show(history.newer())

    @kb.add("c-c")
    @kb.add("c-d")
    def _(event):
        event.app.exit(result=None)

    side = lambda: Window(width=1, char="│", style="class:border")
    body = HSplit([
        _hline("╭", "╮"),
        VSplit([side(), Window(width=1, char=" "), editor, Window(width=1, char=" "), side()],
               height=1),
        _hline("╰", "╯"),
    ])

    app = Application(
        layout=Layout(body, focused_element=editor),
        key_bindings=kb,
        style=Style.from_dict({"border": border, "input": ""}),
        full_screen=False,
    )

    def read_line():
        editor.text = ""
        return app.run()

    return read_line


def _plain_input(message: str):
    try:
        return input(message)
    except EOFError:
        return None
