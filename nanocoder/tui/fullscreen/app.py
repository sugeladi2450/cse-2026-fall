"""Full-screen prompt_toolkit client: input pinned at the bottom, always.

This owns the terminal via the alternate screen. The transcript fills the area
above and scrolls (mouse wheel / PageUp / PageDown / Ctrl-↑ / Ctrl-↓) while the
input box stays fixed on the bottom line and never moves. Tool approvals are
answered in-app with a small keyboard menu. The Rich client in render.py is
still used for non-interactive shells and tests.
"""

import asyncio
import itertools

from prompt_toolkit.application import Application
from prompt_toolkit.filters import Condition
from prompt_toolkit.layout import Layout
from prompt_toolkit.styles import Style
from prompt_toolkit.widgets import TextArea

from .constants import FULLSCREEN_STYLE, PROMPT
from .keys import build_key_bindings
from .layout import make_layout_containers
from .model import TranscriptModel
from .ui import FixedBottomUI
from ..commands import CommandAction, SlashCommandCompleter, handle_command
from ..history import InputHistory
from ..render_bus import install_render
from ..replay import replay_history
from ...agent import events


def run(agent, bus=None, model=None, workspace=None, mode=None, auto_approve: bool = False):
    """Build the fullscreen UI and drive `agent` on its bus until exit.

    `bus` is accepted for symmetry with the assembly in __main__ but the agent
    already carries it as `agent.bus`; approvals and rendering both ride that."""
    transcript = TranscriptModel()
    transcript.banner(model=model, workspace=workspace, mode=mode)
    ui = FixedBottomUI(transcript, auto_approve=auto_approve)
    replay_history(agent.messages, ui)    # a re-attached durable session
    asyncio.run(_run_async(agent, ui))


async def _run_async(agent, ui: FixedBottomUI):
    line_queue = asyncio.Queue()
    history = InputHistory()

    def submit(buff):
        text = buff.text.strip()
        buff.text = ""
        if ui.awaiting_approval:
            if ui.collecting_approval_feedback:
                ui.resolve_approval_feedback(text)
            return True
        if text:
            history.add(text)
            line_queue.put_nowait(text)
        return True

    editor = TextArea(
        prompt=PROMPT,
        multiline=False,
        wrap_lines=False,
        completer=SlashCommandCompleter(),
        complete_while_typing=True,
        accept_handler=submit,
        focus_on_click=True,
        style="class:input",
    )

    mirroring_approval_feedback = False

    def mirror_approval_feedback(_buffer):
        nonlocal mirroring_approval_feedback
        if mirroring_approval_feedback or not ui.collecting_approval_feedback:
            return
        text = editor.text
        if not text:
            return
        mirroring_approval_feedback = True
        try:
            editor.text = ui.append_approval_feedback(text)
            editor.buffer.cursor_position = 0
        finally:
            mirroring_approval_feedback = False

    editor.buffer.on_text_changed += mirror_approval_feedback

    def show_history(text):
        if text is not None:
            editor.text = text
            editor.buffer.cursor_position = len(text)

    kb = build_key_bindings(ui, editor, history, line_queue, show_history)
    root, _transcript_window, _input_box = make_layout_containers(ui, editor)
    app = Application(
        layout=Layout(root, focused_element=editor),
        key_bindings=kb,
        style=Style.from_dict(FULLSCREEN_STYLE),
        full_screen=True,
        mouse_support=Condition(ui.mouse_capture_enabled),
    )
    ui.attach(app, editor, asyncio.get_running_loop())
    install_render(agent.bus, ui, agent.id)

    turn_counter = itertools.count(1)
    app_task = asyncio.create_task(app.run_async())
    try:
        while True:
            line = await line_queue.get()
            if line is None:
                if not app_task.done():
                    try:
                        app.exit()
                    except Exception:
                        pass
                break
            command_action = handle_command(line, ui, agent)
            if command_action is CommandAction.EXIT:
                if not app_task.done():
                    try:
                        app.exit()
                    except Exception:
                        pass
                break
            if command_action is CommandAction.CONTINUE:
                continue
            ui.on_user_message(line)
            try:
                ui.on_turn_start()
                ui.begin_turn()
                # Explicit turn id so Esc can cancel this exact turn on the bus.
                turn_id = f"{agent.id}:{next(turn_counter)}"

                def cancel_turn(tid=turn_id):
                    # Re-arm the loop's cancelled/token check (stops streaming
                    # and off-loop tool dispatch) and fail any in-flight
                    # approval/spawn request the turn is currently awaiting.
                    agent.bus.publish(events.Cancel(turn=tid))
                    agent.bus.cancel_turn(tid)

                ui.bind_cancel(cancel_turn)
                # Rendering + approvals ride the bus (install_render); the turn
                # runs on this loop — its LLM call is off-loop (to_thread) and
                # approvals are loop-native (approve_async), so it won't deadlock
                # with app.run_async().
                turn_task = asyncio.create_task(agent.run(line, turn=turn_id))
                while not turn_task.done():
                    try:
                        await asyncio.wait_for(asyncio.shield(turn_task), timeout=0.25)
                    except asyncio.TimeoutError:
                        ui.refresh_wait_status()
                await turn_task
            except Exception as exc:
                ui.on_error(exc)
    finally:
        if not app_task.done():
            try:
                app.exit()
            except Exception:
                pass
        await app_task
