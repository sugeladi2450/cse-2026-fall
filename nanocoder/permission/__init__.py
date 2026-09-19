"""Permission harness: the single owner of tool-approval policy and state.

`policy` holds the stateless rules (which tools are dangerous, their session
scope, how to read an answer); `PermissionHarness` adds per-session "allow all"
state. The agent loop uses `needs_approval` / `decide`; frontends hold a
`PermissionHarness` to gate prompts.
"""

from .harness import PermissionHarness
from .policy import DANGEROUS, Decision, decide, needs_approval, session_scope

__all__ = [
    "DANGEROUS",
    "Decision",
    "PermissionHarness",
    "decide",
    "needs_approval",
    "session_scope",
]
