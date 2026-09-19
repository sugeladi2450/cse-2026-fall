"""CLI entry point: assemble a bus + root Agent + subscribers, run one task.

The CLI and the TUI share one substrate now: a Bus with a root Agent actor.
The CLI attaches a stdout renderer (root events only), a terminal/auto
permission responder, and the session logger, then runs the agent to
completion on an asyncio loop. Config resolution (CLI > config > env) is
unchanged from before.
"""

import argparse
import asyncio
import os
import sys

from .agent import events
from .agent.llm import MockBackend, OpenAIBackend
from .agent.loop import Agent
from .bus import Bus
from .config import load_config
from .permission import policy
from .session_log import SessionLogger
from .skills import Skills
from .subagents import Subagents, install_subagents
from .tools.registry import build_registry


def make_agent(workspace, bus, mock=None, model=None, base_url=None, api_key=None,
               max_steps=20, bash_timeout=30.0, plan_mode=False, skills=None,
               skills_catalog="", store=None):
    """Build the root Agent (id 'root') bound to `bus`.

    `skills` is the Skills() instance the registry's load_skill tool draws
    on; pass one in to share it with the caller's catalog text instead of
    discovering skills twice. Defaults to a fresh Skills() if omitted.
    `store` is the message-history store (None → the agent's InMemoryStore)."""
    skills = skills if skills is not None else Skills()
    registry = build_registry(workspace, bash_timeout=bash_timeout, skills=skills,
                              with_task=True)
    backend = MockBackend.from_file(mock) if mock else \
        OpenAIBackend(model=model, base_url=base_url, api_key=api_key)
    return Agent("root", bus, backend, registry, max_steps=max_steps,
                 plan_mode=plan_mode,
                 skills_catalog=skills_catalog or skills.catalog_text(),
                 store=store)


def _raft_store(raft_spec, session_id):
    """Build a RaftMessageStore from a --raft "h0:p0,h1:p1,..." spec + --session id.
    Imports are lazy so core nanoCoder never needs the msgpack dependency; a
    missing dependency exits with the install command instead of a traceback."""
    try:
        from .message_store import RaftMessageStore
        from .raft_client import RaftKvClient
    except ModuleNotFoundError as e:
        raise SystemExit(
            f"durable sessions need the '{e.name}' dependency: run commands "
            f"with 'uv run' (or pip install {e.name})") from e
    nodes = []
    for tok in raft_spec.split(","):
        host, _, port = tok.rpartition(":")
        nodes.append((host, int(port)))
    return RaftMessageStore(RaftKvClient(nodes), session_id)


def _install_stdout_renderer(bus, root_id):
    """Print root-agent text + tool activity to stdout; ignore subagent events."""
    def render(e):
        if isinstance(e, events.TextDelta):
            sys.stdout.write(e.text); sys.stdout.flush()
        elif isinstance(e, events.ToolCall):
            print(f"\n[tool] {e.name} {e.args}")
        elif isinstance(e, events.ToolResult):
            print(f"[obs] {e.observation}")
    bus.subscribe(lambda e: getattr(e, "frm", "") == root_id, render)


def _install_permission_responder(bus, approve):
    """Answer the agent's escalated PermissionRequest events by consulting
    approve(tool, args) and replying with an ApprovalDecision.

    PermissionRequest (not ApprovalNeeded) is the responder's input: the
    PermissionRouter owns ApprovalNeeded and auto-approves allow-listed child
    tools itself, escalating only what the user actually needs to see."""
    def respond(req):
        decision = policy.decide(approve(req.tool, req.args))
        bus.reply(req, events.ApprovalDecision(
            allowed=decision.allowed, feedback=decision.feedback))
    bus.subscribe(lambda e: isinstance(e, events.PermissionRequest), respond)


async def run_cli(agent, bus, task, approve, subagents=None):
    """Attach CLI subscribers (render, permission, subagent runtime+router)
    and drive the root agent to completion."""
    subagents = subagents if subagents is not None else Subagents()
    install_subagents(bus, agent, subagents)      # supervision + spawn + router
    _install_stdout_renderer(bus, agent.id)
    _install_permission_responder(bus, approve)   # answers PermissionRequest
    return await agent.run(task)


def main(argv=None):
    parser = argparse.ArgumentParser(prog="nanocoder")
    parser.add_argument("--task", required=True, help="Task for the agent to perform.")
    parser.add_argument("--workspace", default=".", help="Workspace root for tools.")
    parser.add_argument("--mock", help="Path to a JSON tool-call script (deterministic).")
    parser.add_argument("--config", help="Path to a JSON config file (overrides discovery).")
    parser.add_argument("--model", help="Model name (overrides config).")
    parser.add_argument("--base-url", help="OpenAI-compatible base URL (overrides config/env).")
    parser.add_argument("--api-key", help="API key (overrides config/env).")
    parser.add_argument("--max-steps", type=int, default=20)
    parser.add_argument("--bash-timeout", type=float, default=30.0)
    parser.add_argument(
        "--yes",
        action="store_true",
        help="Auto-approve dangerous tools in real CLI mode.",
    )
    parser.add_argument(
        "--plan",
        action="store_true",
        help="Read-only plan mode: block file/shell mutations.",
    )
    parser.add_argument(
        "--raft",
        help="Durable message history: comma-separated nanoraftd nodes "
             "(h0:p0,h1:p1,h2:p2). Overrides the config's \"raft\" key; "
             "requires --session.",
    )
    parser.add_argument(
        "--session",
        help="Session id for durable history; re-attach to resume it. "
             "Needs raft nodes from --raft or the config's \"raft\" key.",
    )
    args = parser.parse_args(argv)
    if args.raft and not args.session:
        parser.error("--raft requires --session <id>")

    config = load_config(args.config)
    raft_spec = args.raft or config.get("raft")
    if args.session and not raft_spec:
        parser.error('--session needs raft nodes: pass --raft or set "raft" in the config')
    model = args.model or config.get("model")
    base_url = args.base_url or config.get("base_url") or os.environ.get("OPENAI_BASE_URL")
    api_key = args.api_key or config.get("api_key") or os.environ.get("OPENAI_API_KEY")
    if not args.mock and not (model and base_url):
        parser.error(
            "real mode needs a model and base_url (via flag, config, or env); "
            "or pass --mock for a deterministic run"
        )

    log_dir = config.get("log_dir") or os.path.join(args.workspace, "logs")
    logger = SessionLogger(log_dir, model=model, task=args.task)

    bus = Bus()
    logger.subscribe_to(bus)
    skills = Skills()
    store = _raft_store(raft_spec, args.session) if args.session else None
    agent = make_agent(args.workspace, bus, mock=args.mock, model=model,
                       base_url=base_url, api_key=api_key, max_steps=args.max_steps,
                       bash_timeout=args.bash_timeout, plan_mode=args.plan,
                       skills=skills, store=store)
    subagents = Subagents()
    agent.system_prompt += ("\n\n## Subagents\nDelegate a subtask with "
                            "task(subagent_type, description):\n" + subagents.catalog_text())

    def approve_tool(name, tool_args):
        if args.mock or args.yes:
            return True
        if not sys.stdin.isatty():
            return False
        try:
            answer = input(f"Run {name}? [y/N] ")
        except EOFError:
            return False
        return answer.strip().lower() in {"y", "yes"}

    result = asyncio.run(run_cli(agent, bus, args.task, approve_tool, subagents))
    print("\n" + result)
    m = agent.metrics
    print(f"\n{m.prompt_tokens} prompt tokens • {m.completion_tokens} completion • "
          f"{m.wall_s:.2f}s • {m.model or model}")
    logger.finalize({
        "tool_calls": sum(1 for x in agent.messages if x.get("role") == "tool"),
        "total_tokens": m.total_tokens,
    })


if __name__ == "__main__":
    main()
