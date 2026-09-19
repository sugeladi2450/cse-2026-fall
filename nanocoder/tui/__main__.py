"""`python -m nanocoder.tui`: assemble an agent and start the CSE REPL.

Backend resolution (model / base_url / api_key) mirrors nanocoder.main:
CLI flag > config file > env. `--mock` runs a deterministic, network-free
session. `read_line` / `console` are injectable for testing.
"""

import argparse
import os
import sys

from ..bus import Bus
from ..config import load_config
from ..main import _raft_store, make_agent
from ..subagents import Subagents, install_subagents
from . import app, fullscreen
from .prompt import make_reader
from .render import RichUI


def main(argv=None, read_line=None, console=None):
    parser = argparse.ArgumentParser(prog="nanocoder.tui")
    parser.add_argument("--workspace", default=".", help="Workspace root for tools.")
    parser.add_argument("--mock", help="Path to a JSON tool-call script (deterministic).")
    parser.add_argument("--config", help="Path to a JSON config file (overrides discovery).")
    parser.add_argument("--model", help="Model name (overrides config).")
    parser.add_argument("--base-url", help="OpenAI-compatible base URL (overrides config/env).")
    parser.add_argument("--api-key", help="API key (overrides config/env).")
    parser.add_argument("--max-steps", type=int, default=20)
    parser.add_argument("--bash-timeout", type=float, default=30.0)
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

    bus = Bus()
    store = _raft_store(raft_spec, args.session) if args.session else None
    agent = make_agent(
        workspace=args.workspace,
        bus=bus,
        mock=args.mock,
        model=model,
        base_url=base_url,
        api_key=api_key,
        max_steps=args.max_steps,
        bash_timeout=args.bash_timeout,
        store=store,
    )
    subagents = Subagents()
    agent.system_prompt += ("\n\n## Subagents\nDelegate a subtask with "
                            "task(subagent_type, description):\n" + subagents.catalog_text())
    install_subagents(bus, agent, subagents)  # supervision + spawn + approval router

    if read_line is None and console is None and sys.stdin.isatty():
        mode = "mock" if args.mock else "real"
        fullscreen.run(
            agent,
            bus=bus,
            model=model,
            workspace=args.workspace,
            mode=mode,
            auto_approve=bool(args.mock),
        )
    else:
        app.run(
            agent,
            RichUI(console, auto_approve=bool(args.mock)),
            read_line=read_line or make_reader(),
        )


if __name__ == "__main__":
    main()
