"""Subagents: file-defined restricted agents plus the runtime that spawns them.

A subagent is a `.nanocoder/agents/<name>.md` file — frontmatter (name,
description, optional tools allow-list, optional model) and a body that is the
child's system prompt. Discovery parallels skills.py. The built-in `general`
subagent is always available. This module also hosts the Supervision registry,
the SpawnRuntime (turns Spawn requests into child agents), and the
PermissionRouter (approval escalation) — see Task 12-14.
"""

import asyncio
import itertools
import os
from pathlib import Path

from .agent import events
from .agent.loop import Agent
from .bus import RequestCancelled
from .skills import _parse_frontmatter

GENERAL_PROMPT = (
    "You are a general-purpose subagent. Complete the delegated task using the "
    "available tools, then reply with a concise summary of what you did and found."
)


class Subagent:
    def __init__(self, name, description, prompt, tools, model):
        self.name = name
        self.description = description
        self.prompt = prompt
        self.tools = tools          # list[str] | None  (None = all parent tools)
        self.model = model          # str | None


GENERAL = Subagent("general", "General-purpose subagent.", GENERAL_PROMPT, None, None)


def _parse_tools(raw: str):
    """Parse a `tools:` frontmatter value into a list, or None if absent."""
    raw = (raw or "").strip().strip("[]").strip()
    if not raw:
        return None
    return [t.strip() for t in raw.replace(",", " ").split() if t.strip()]


def _split_body(text: str) -> str:
    """Return the body following the leading frontmatter fence, or "" if malformed.

    Mirrors the closing-fence boundary `skills._parse_frontmatter` uses
    (`text.find("\\n---", 3)`) rather than splitting on the bare substring
    `"---"`, so a `---` inside a frontmatter value can't corrupt the body.
    If `text` has no opening fence, the whole text is the body. If it has an
    opening fence but no closing fence, the file is malformed and the body is
    empty (raw frontmatter lines are never leaked into the prompt).
    """
    if not text.startswith("---"):
        return text.strip()
    end = text.find("\n---", 3)
    if end == -1:
        return ""
    newline = text.find("\n", end + 1)
    if newline == -1:
        return ""
    return text[newline + 1:].strip()


def discover_subagents(roots) -> list:
    """Return Subagents found as <root>/*.md across roots (skills-style)."""
    found = {}
    for root in roots:
        root = Path(root).expanduser()
        if not root.is_dir():
            continue
        for md in sorted(root.glob("*.md")):
            text = md.read_text()
            meta = _parse_frontmatter(text)
            name = meta.get("name") or md.stem
            body = _split_body(text)
            found.setdefault(name, Subagent(
                name, meta.get("description", ""), body,
                _parse_tools(meta.get("tools", "")), meta.get("model") or None))
    return list(found.values())


DEFAULT_ROOTS = [".nanocoder/agents", os.path.expanduser("~/.nanocoder/agents")]


class Subagents:
    def __init__(self, roots=None):
        self._subs = {"general": GENERAL}
        for s in discover_subagents(roots or DEFAULT_ROOTS):
            self._subs[s.name] = s

    def get(self, name):
        """The Subagent named `name`, or None."""
        return self._subs.get(name)

    def catalog_text(self) -> str:
        """One `- name: description` line per subagent (for the system prompt)."""
        return "\n".join(f"- {s.name}: {s.description}" for s in self._subs.values())


_CHILD_IDS = itertools.count(1)


class Supervision:
    """The supervision tree + per-agent allow-lists, shared by SpawnRuntime
    (writes) and PermissionRouter (reads)."""

    def __init__(self):
        self._parent = {}       # child_id -> parent_id
        self._allow = {}        # agent_id -> set(tool names)

    def register(self, child_id, parent_id, allow):
        """Record `child_id`'s parent and its allowed tool names."""
        self._parent[child_id] = parent_id
        self._allow[child_id] = set(allow)

    def parent(self, agent_id):
        """The parent id registered for `agent_id`, or None."""
        return self._parent.get(agent_id)

    def allows(self, agent_id, tool):
        """Whether `tool` is in `agent_id`'s allow-list."""
        return tool in self._allow.get(agent_id, set())


class SpawnRuntime:
    """Turns Spawn requests into restricted child agents.

    On Spawn{spec, task} it looks up the subagent definition, builds a child
    registry (parent tools narrowed to the allow-list, never `task`), registers
    supervision (parent + allow-list), runs the child, and replies with its
    final text. Unknown subagents reply with an error observation."""

    def __init__(self, bus, parent_agent, subagents, supervision,
                 child_backend_factory=None):
        self.bus = bus
        self.parent = parent_agent
        self.subagents = subagents
        self.supervision = supervision
        # default: reuse the parent's backend (per-agent model is future work)
        self.child_backend_factory = child_backend_factory or (
            lambda spec: parent_agent.backend)
        self._tasks = set()
        bus.subscribe(lambda e: isinstance(e, events.Spawn), self._on_spawn)

    def _on_spawn(self, req):
        task = asyncio.create_task(self._run(req))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    def _child_registry(self, spec):
        """Parent registry narrowed to the subagent's allow-list (or all tools
        minus task when the definition omits tools)."""
        all_names = {s["function"]["name"] for s in self.parent.registry.schemas()}
        allowed = set(spec.tools) if spec.tools is not None else all_names
        allowed.discard("task")
        return self.parent.registry.subset(allowed), allowed

    async def _run(self, req):
        spec = self.subagents.get(req.spec)
        if spec is None:
            self.bus.reply(req, events.SpawnResult(
                final_text=f"error: unknown subagent {req.spec!r}"))
            return
        child = None
        try:
            registry, allowed = self._child_registry(spec)
            child_id = f"{self.parent.id}/{spec.name}#{next(_CHILD_IDS)}"
            self.supervision.register(child_id, self.parent.id, allowed)
            child = Agent(child_id, self.bus, self.child_backend_factory(spec),
                          registry, system_prompt=spec.prompt,
                          max_steps=self.parent.max_steps)
            final = await child.run(req.task, turn=req.turn)
        except Exception as e:
            final = f"error: subagent failed: {e}"
        finally:
            if child is not None:
                self.parent.metrics = self.parent.metrics.merge(child.metrics)
        self.bus.reply(req, events.SpawnResult(final_text=final))


class PermissionRouter:
    """Approval escalation chain.

    Every agent that hits a dangerous tool publishes ApprovalNeeded. If the
    agent has a supervisor and the tool is in the agent's allow-list, the
    supervisor auto-approves it (the user is not bothered). Otherwise the
    request escalates to the user as a PermissionRequest; the reply flows back
    to the original ApprovalNeeded by correlation id."""

    def __init__(self, bus, supervision):
        """Wire up escalation for `bus`, consulting `supervision` for allow-lists."""
        self.bus = bus
        self.supervision = supervision
        self._tasks = set()
        bus.subscribe(lambda e: isinstance(e, events.ApprovalNeeded), self._on_needed)

    def _on_needed(self, req):
        """Route an ApprovalNeeded: auto-approve if `req.frm` has a supervisor
        that allows `req.tool`, else schedule escalation to the user."""
        agent_id, tool = req.frm, req.tool
        if (self.supervision.parent(agent_id) is not None
                and self.supervision.allows(agent_id, tool)):
            self.bus.reply(req, events.ApprovalDecision(allowed=True))
            return
        task = asyncio.create_task(self._escalate(req))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def _escalate(self, req):
        """Ask the user for a decision on `req` and reply the original
        ApprovalNeeded with the user's allowed/feedback. If the turn is
        cancelled while the escalated PermissionRequest is awaiting the user,
        return quietly (the original ApprovalNeeded is failed by
        cancel_turn too, so there is nothing left to reply)."""
        try:
            decision = await self.bus.request(events.PermissionRequest(
                frm=req.frm, turn=req.turn, tool=req.tool, args=req.args))
        except RequestCancelled:
            return
        self.bus.reply(req, events.ApprovalDecision(
            allowed=decision.allowed, feedback=decision.feedback))


def install_subagents(bus, parent_agent, subagents, child_backend_factory=None):
    """Wire Spawn runtime + PermissionRouter onto the bus for `parent_agent`.
    Returns (Supervision, SpawnRuntime, PermissionRouter)."""
    supervision = Supervision()
    runtime = SpawnRuntime(bus, parent_agent, subagents, supervision,
                           child_backend_factory)
    router = PermissionRouter(bus, supervision)
    return supervision, runtime, router
