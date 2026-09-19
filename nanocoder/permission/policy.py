"""Permission policy: which tools need approval, and how an answer is read.

Pure, stateless functions — the static rules of the harness. The dangerous
set decides what must be approved; `session_scope` groups tools for the
"allow all during this session" choice; `decide` turns a frontend's tri-state
answer (True / False / feedback string) into a `Decision`.
"""

from dataclasses import dataclass

DANGEROUS = {"write_file", "edit_file", "bash"}
PLAN_MODE_BLOCKED = {"write_file", "edit_file", "bash"}


def needs_approval(tool_name: str) -> bool:
    return tool_name in DANGEROUS


def blocked_in_plan_mode(tool_name: str) -> bool:
    """True for tools that mutate state and are disallowed in read-only plan mode."""
    return tool_name in PLAN_MODE_BLOCKED


def session_scope(tool_name: str) -> str:
    """The bucket a tool falls in for a session-wide "allow all" decision."""
    if tool_name in {"edit_file", "write_file"}:
        return "edits"
    if tool_name == "bash":
        return "commands"
    return "tools"


@dataclass(frozen=True)
class Decision:
    """Outcome of an approval: run it, or reject (optionally with feedback)."""

    allowed: bool
    feedback: str = ""


def decide(answer) -> Decision:
    """Read a frontend's answer: True runs, a non-empty string rejects with
    feedback for the model, anything else is a plain rejection."""
    if answer is True:
        return Decision(allowed=True)
    if isinstance(answer, str) and answer.strip():
        return Decision(allowed=False, feedback=answer.strip())
    return Decision(allowed=False)
