# nanocoder/agent/loop.py  (rewrite)
"""The Agent actor.

An Agent runs a ReAct loop as an asyncio coroutine. It publishes observation
events (TurnStarted / TextDelta / ToolCall / ToolResult / LLMCall / TurnDone)
tagged with its own id and turn, and it awaits bus.request(...) for the two
operations that need an answer: spawning a subagent (Spawn) and getting a
dangerous tool approved (ApprovalNeeded). The blocking LLM call runs in a
worker thread so the event loop (and the bus/TUI) stay responsive.
"""

import asyncio
import itertools

from . import events
from .cancel import CancelToken
from .messages import parse_tool_call, tool_observation
from ..bus import RequestCancelled
from ..message_store import InMemoryStore
from ..metrics import LLMMetrics
from ..permission import policy

DEFAULT_SYSTEM_PROMPT = (
    "You are nanoCoder, a minimal coding agent. You operate inside a workspace "
    "using the provided tools: read_file, write_file, edit_file, and bash. "
    "Use tools to inspect and modify files, then reply with a short summary when done."
)

_TURN_IDS = itertools.count(1)


class Agent:
    def __init__(self, agent_id, bus, backend, registry, system_prompt=None,
                 max_steps=20, plan_mode=False, skills_catalog="", store=None):
        self.id = agent_id
        self.bus = bus
        self.backend = backend
        self.registry = registry
        base = system_prompt or DEFAULT_SYSTEM_PROMPT
        if skills_catalog:
            base += ("\n\n## Skills\nLoad a skill with load_skill(name) when relevant:\n"
                     + skills_catalog)
        self.system_prompt = base
        self.skills_catalog = skills_catalog
        self.plan_mode = plan_mode
        self.max_steps = max_steps
        self.metrics = LLMMetrics()
        # Message history lives behind a store interface. The default InMemoryStore is
        # today's behavior (empty list, no persistence); a RaftMessageStore seeds
        # self.messages from a durable session so a re-attached nanoCoder resumes.
        self._store = store or InMemoryStore()
        self.messages = self._store.load()
        self.turn = ""
        self.cancelled = False
        self._cancel_token = None

    def _on_cancel(self, event):
        self.cancelled = True
        self._cancel_token.cancel()

    def _after_backend_call(self):
        """Fold the backend's per-call metrics into the session total and
        publish an LLMCall event (metrics/logger subscribers consume it)."""
        m = getattr(self.backend, "last_metrics", None)
        if m is not None:
            self.metrics = self.metrics.merge(m)
            self.bus.publish(events.LLMCall(
                frm=self.id, turn=self.turn, model=m.model,
                prompt_tokens=m.prompt_tokens, completion_tokens=m.completion_tokens,
                total_tokens=m.total_tokens))

    async def _stream(self):
        """Run the blocking backend.stream() in a thread, bridging each text
        delta back onto the loop as a TextDelta. Returns the assistant msg dict."""
        loop = asyncio.get_running_loop()

        def emit(text):
            loop.call_soon_threadsafe(
                self.bus.publish,
                events.TextDelta(frm=self.id, turn=self.turn, text=text))

        def work():
            gen = self.backend.stream(self.messages, self.registry.schemas())
            try:
                while True:
                    _, text = next(gen)
                    emit(text)
            except StopIteration as e:
                return e.value

        msg = await asyncio.to_thread(work)
        self._after_backend_call()
        return msg

    def _remember(self, msg):
        """Append a message to the live history and the durable store together."""
        self.messages.append(msg)
        self._store.append(msg)

    def _record(self, tool_call_id, observation):
        self._remember(tool_observation(tool_call_id, observation))

    def _finish_cancelled(self, note="cancelled by user"):
        """Keep messages valid and publish TurnCancelled."""
        self._remember({"role": "assistant", "content": f"({note})"})
        self.bus.publish(events.TurnCancelled(frm=self.id, turn=self.turn, note=note))
        return f"({note})"

    async def _dispatch(self, tc_id, name, args):
        """Run one non-spawn tool call. Blocked tools in plan mode return the
        read-only notice; dangerous tools go through approval; the rest run."""
        if self.plan_mode and policy.blocked_in_plan_mode(name):
            return ("plan mode: read-only — propose changes in your plan, "
                    "do not modify files")
        if policy.needs_approval(name):
            return await self._approved_dispatch(name, args)
        return await asyncio.to_thread(self.registry.dispatch, name, args, self._cancel_token)

    async def _approved_dispatch(self, name, args):
        """Ask for approval via the bus; run on allow, else record a rejection."""
        decision = await self.bus.request(events.ApprovalNeeded(
            frm=self.id, turn=self.turn, tool=name, args=args))
        if decision.allowed:
            return await asyncio.to_thread(self.registry.dispatch, name, args, self._cancel_token)
        obs = f"user rejected: {name}"
        if decision.feedback:
            obs += f"\nuser feedback: {decision.feedback}"
        return obs

    async def run(self, task, turn=None):
        """Drive one user turn to completion as a coroutine; return final text.

        Publishes TurnStarted, then streams the model. On tool_calls it routes
        each call (Task 6-7); with no tool_calls it publishes TurnDone and
        returns the final text. Subscribes to Cancel for this turn and stops at
        the next boundary if it fires."""
        self.turn = turn or f"{self.id}:{next(_TURN_IDS)}"
        self.cancelled = False
        self._cancel_token = CancelToken()
        self.bus.subscribe(
            lambda e: isinstance(e, events.Cancel) and e.turn == self.turn,
            self._on_cancel)
        self.bus.publish(events.TurnStarted(frm=self.id, turn=self.turn))
        if not self.messages:
            self._remember({"role": "system", "content": self.system_prompt})
        self._remember({"role": "user", "content": task})

        try:
            for _ in range(self.max_steps):
                if self.cancelled:
                    return self._finish_cancelled()
                msg = await self._stream()
                self._remember(msg)
                tool_calls = msg.get("tool_calls")
                if not tool_calls:
                    final = msg.get("content") or ""
                    self.bus.publish(events.TurnDone(
                        frm=self.id, turn=self.turn, final_text=final))
                    return final
                for index, tc in enumerate(tool_calls):
                    name, args, error = parse_tool_call(tc)
                    self.bus.publish(events.ToolCall(
                        frm=self.id, turn=self.turn, id=tc["id"],
                        name=name, args=args or {}))
                    if error is not None:
                        obs = error
                    else:
                        obs = await self._route(tc["id"], name, args)
                    self._record(tc["id"], obs)
                    self.bus.publish(events.ToolResult(
                        frm=self.id, turn=self.turn, id=tc["id"],
                        name=name, observation=obs))
                    if self.cancelled:
                        for pending in tool_calls[index + 1:]:
                            self._record(pending["id"], "cancelled by user")
                        return self._finish_cancelled()
            final = f"(reached max steps: {self.max_steps})"
            self.bus.publish(events.TurnDone(
                frm=self.id, turn=self.turn, final_text=final))
            return final
        except RequestCancelled:
            return self._finish_cancelled()
        finally:
            self.bus.unsubscribe(self._on_cancel)

    async def _route(self, tc_id, name, args):
        """task -> spawn a subagent; everything else -> _dispatch."""
        if name == "task":
            return await self._spawn(args)
        return await self._dispatch(tc_id, name, args)

    async def _spawn(self, args):
        """Delegate to a subagent: request Spawn and return its final text."""
        result = await self.bus.request(events.Spawn(
            frm=self.id, turn=self.turn,
            spec=args.get("subagent_type", ""),
            task=args.get("description", "")))
        return result.final_text
