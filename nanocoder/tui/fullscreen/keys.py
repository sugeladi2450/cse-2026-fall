from prompt_toolkit.filters import Condition
from prompt_toolkit.key_binding import KeyBindings


def build_key_bindings(ui, editor, history, line_queue, show_history):
    kb = KeyBindings()
    active = Condition(lambda: not ui.awaiting_approval)
    approval_menu = Condition(lambda: ui.awaiting_approval and not ui.collecting_approval_feedback)
    approval_feedback = Condition(lambda: ui.collecting_approval_feedback)
    running = Condition(ui.can_cancel)

    @kb.add("up", filter=active)
    def _(event):
        show_history(history.older())

    @kb.add("down", filter=active)
    def _(event):
        show_history(history.newer())

    @kb.add("pageup", filter=active)
    def _(event):
        ui.scroll_up(max(1, ui.visible_lines - 1))

    @kb.add("pagedown", filter=active)
    def _(event):
        ui.scroll_down(max(1, ui.visible_lines - 1))

    @kb.add("c-up", filter=active)
    def _(event):
        ui.scroll_up(3)

    @kb.add("c-down", filter=active)
    def _(event):
        ui.scroll_down(3)

    @kb.add("f2", filter=active)
    def _(event):
        ui.toggle_selection_mode()

    @kb.add("up", filter=approval_menu)
    def _(event):
        ui.move_approval_selection(-1)

    @kb.add("down", filter=approval_menu)
    def _(event):
        ui.move_approval_selection(1)

    @kb.add("enter", filter=approval_menu)
    def _(event):
        ui.resolve_selected_approval()

    @kb.add("tab", filter=approval_menu)
    def _(event):
        editor.text = ""
        editor.buffer.cursor_position = 0
        ui.enter_approval_feedback_mode()

    @kb.add("backspace", filter=approval_feedback)
    @kb.add("c-h", filter=approval_feedback)
    def _(event):
        editor.text = ui.delete_approval_feedback_char()
        editor.buffer.cursor_position = 0

    @kb.add("1", filter=approval_menu)
    @kb.add("y", filter=approval_menu)
    def _(event):
        ui.resolve_approval(True)

    @kb.add("2", filter=approval_menu)
    @kb.add("escape", "m", filter=approval_menu)
    def _(event):
        ui.resolve_approval(True, allow_session=True)

    @kb.add("3", filter=approval_menu)
    @kb.add("n", filter=approval_menu)
    @kb.add("escape", filter=approval_menu)
    def _(event):
        ui.resolve_approval(False)

    @kb.add("escape", filter=approval_feedback)
    def _(event):
        ui.cancel_approval_feedback()

    @kb.add("escape", filter=running)
    def _(event):
        ui.request_cancel()

    @kb.add("c-c")
    @kb.add("c-d")
    def _(event):
        ui.request_exit()
        line_queue.put_nowait(None)
        event.app.exit()

    return kb
