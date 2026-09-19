"""Replay a restored session's transcript through a UI.

When a nanoCoder window dies and a fresh one attaches to the same durable
--session, the agent seeds its history from the store; this module re-renders
that history through the same ui.on_* callbacks live turns use, so the new
window opens looking like the old one never died. Both TUI clients (RichUI and
FixedBottomUI) implement the callback set this walks.
"""

from ..agent.messages import parse_tool_call


def replay_history(messages, ui):
    """Render restored `messages` (OpenAI-shaped dicts) through `ui`.

    Skips the system prompt, prints a one-line `restored N messages` status,
    then replays user lines, assistant text, tool calls and tool results in
    order via the same callbacks live rendering uses. Tool-result lines get
    their tool's name by mapping tool_call_id from the preceding assistant
    message. No-op on an empty history (a fresh session).
    """
    shown = [m for m in messages if m.get("role") != "system"]
    if not shown:
        return
    ui.on_status(f"restored {len(shown)} messages from the durable session")
    names = {}                              # tool_call_id -> tool name
    for m in shown:
        role = m.get("role")
        if role == "user":
            ui.on_user_message(m.get("content") or "")
        elif role == "assistant":
            if m.get("content"):
                ui.on_text_delta(m["content"])
            calls = m.get("tool_calls") or []
            for tc in calls:
                name, args, _err = parse_tool_call(tc)
                names[tc.get("id")] = name
                ui.on_tool_call(name, args)
            if not calls:
                ui.on_turn_done(m.get("content") or "")
        elif role == "tool":
            ui.on_tool_result(names.get(m.get("tool_call_id"), ""),
                              m.get("content") or "")
