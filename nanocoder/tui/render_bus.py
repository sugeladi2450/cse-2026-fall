"""Attach a UI to the bus: render only the root agent's events, and answer
escalated PermissionRequests via ui.approve()/ui.approve_async().

Subagent observation events (frm != root_id) ride the bus for the logger and
metrics but are deliberately not shown — the subagent is opaque in the
transcript; it surfaces only through the root's task ToolCall/ToolResult.
"""

import asyncio

from ..agent import events
from ..permission import policy


def install_render(bus, ui, root_id):
    """Wire `ui` to render root-agent observation events and answer approvals.

    Only events with frm == root_id reach `ui`; subagent events are ignored.
    PermissionRequest is answered via `ui.approve_async` when the UI provides
    one (fullscreen), else the synchronous `ui.approve` (Rich UI, tests).
    """
    def render(e):
        """Dispatch a root-agent observation event `e` to the matching `ui.on_*`
        callback; events from any other agent (subagent chatter) are ignored."""
        if getattr(e, "frm", "") != root_id:
            return
        if isinstance(e, events.TextDelta):
            ui.on_text_delta(e.text)
        elif isinstance(e, events.ToolCall):
            ui.on_tool_call(e.name, e.args)
        elif isinstance(e, events.ToolResult):
            ui.on_tool_result(e.name, e.observation)
        elif isinstance(e, events.TurnDone):
            ui.on_turn_done(e.final_text)
        elif isinstance(e, events.TurnCancelled):
            ui.on_turn_cancelled(e.note)

    bus.subscribe(lambda e: isinstance(e, (events.TextDelta, events.ToolCall,
                                           events.ToolResult, events.TurnDone,
                                           events.TurnCancelled)), render)

    pending = set()  # strong refs to in-flight ask() tasks, so they can't be GC'd

    def on_permission(req):
        """Answer a PermissionRequest via `ui.approve_async` (fullscreen) or the
        synchronous `ui.approve` (Rich UI, tests); always replies on the bus,
        even if the async path raises, so `bus.request` never hangs."""
        if hasattr(ui, "approve_async"):
            async def ask():
                try:
                    answer = await ui.approve_async(req.tool, req.args)
                    d = policy.decide(answer)
                    decision = events.ApprovalDecision(allowed=d.allowed, feedback=d.feedback)
                except Exception as e:
                    decision = events.ApprovalDecision(allowed=False, feedback=f"approval error: {e}")
                bus.reply(req, decision)
            task = asyncio.create_task(ask())
            pending.add(task)
            task.add_done_callback(pending.discard)
        else:
            answer = ui.approve(req.tool, req.args)
            d = policy.decide(answer)
            bus.reply(req, events.ApprovalDecision(allowed=d.allowed, feedback=d.feedback))

    bus.subscribe(lambda e: isinstance(e, events.PermissionRequest), on_permission)
