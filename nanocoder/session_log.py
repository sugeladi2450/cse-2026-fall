"""Synchronous per-run session log.

One directory per agent run under a base dir: `events.jsonl` (one JSON object
per event), `llm.log` (human timeline), `session.json` (metadata + aggregate
counts). A `latest` symlink in the base dir points at the newest run. No async
transport, no artifact spill (minimal altitude).
"""

import json
import time
from dataclasses import asdict
from pathlib import Path


def _event_dict(event) -> dict:
    """Serialize an events.py dataclass to a plain dict tagged with its type."""
    data = {"type": type(event).__name__}
    data.update(asdict(event))
    return data


class SessionLogger:
    def __init__(self, base_dir, model: str = None, task: str = None):
        self.base = Path(base_dir)
        self.dir = self.base / time.strftime("%Y%m%d-%H%M%S")
        self.dir.mkdir(parents=True, exist_ok=True)
        self._events = (self.dir / "events.jsonl").open("a")
        self._human = (self.dir / "llm.log").open("a")
        self._meta = {"model": model, "task": task, "started": time.time()}
        self._link_latest()

    def _link_latest(self):
        """Point <base>/latest at this run's directory (best-effort)."""
        link = self.base / "latest"
        try:
            if link.is_symlink() or link.exists():
                link.unlink()
            link.symlink_to(self.dir.name)
        except OSError:
            pass  # symlinks are a convenience, not a requirement

    def subscribe_to(self, bus):
        """Log every event on the bus (full firehose, including subagents)."""
        bus.subscribe(lambda e: True, self.log_event)

    def log_event(self, event):
        """Append one structured event line and a human-readable mirror line."""
        self._events.write(json.dumps(_event_dict(event)) + "\n")
        self._events.flush()
        self._human.write(f"{type(event).__name__}: {getattr(event, 'name', '')}\n")
        self._human.flush()

    def log_llm_call(self, metrics):
        """Record an LLM call's token/timing metrics as a structured event line."""
        self._events.write(json.dumps({"type": "LLMCall", "model": metrics.model,
                                       "total_tokens": metrics.total_tokens,
                                       "wall_s": metrics.wall_s}) + "\n")
        self._events.flush()

    def finalize(self, counts: dict):
        """Write session.json with metadata + aggregate counts and close files."""
        session = dict(self._meta)
        session["ended"] = time.time()
        session["counts"] = counts
        (self.dir / "session.json").write_text(json.dumps(session, indent=2))
        self._events.close()
        self._human.close()
