"""The CSE REPL: read a line, drive one agent turn on the bus, render, repeat.

`read_line` is injectable so the loop is testable without real stdin. Rendering
is wired once via install_render, which subscribes `ui` to the agent's bus and
answers escalated approvals. Each turn runs the agent coroutine to completion
with asyncio.run; a turn that raises is reported via ui.on_error and the REPL
keeps going.
"""

import asyncio

from .commands import CommandAction, handle_command
from .render_bus import install_render
from .replay import replay_history


def run(agent, ui, read_line=None):
    """Run the read/drive/render REPL against `agent` until EOF or /exit.

    Wires `ui` to `agent.bus` once (root events + approvals), then drives one
    agent turn per submitted line. Slash commands are handled without a turn.
    """
    read_line = read_line or (lambda: input("› "))
    ui.banner()
    install_render(agent.bus, ui, agent.id)
    replay_history(agent.messages, ui)    # a re-attached durable session

    while True:
        try:
            line = read_line()
        except (EOFError, KeyboardInterrupt):
            break
        if line is None:
            break
        line = line.strip()
        if not line:
            continue
        command_action = handle_command(line, ui, agent)
        if command_action is CommandAction.EXIT:
            break
        if command_action is CommandAction.CONTINUE:
            continue
        ui.on_user_message(line)
        try:
            asyncio.run(agent.run(line))
        except Exception as exc:  # keep the REPL alive on any turn failure
            ui.on_error(exc)
