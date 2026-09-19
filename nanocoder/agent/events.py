"""Events on the bus.

Every event carries an envelope (frm/to/corr/turn) so subscribers can filter
by origin and the bus can correlate a request with its reply and fail a turn's
in-flight requests on Cancel. Observation events are published fan-out; the
request/reply pairs (Spawn/SpawnResult, ApprovalNeeded/ApprovalDecision,
PermissionRequest/ApprovalDecision) travel through Bus.request()/reply().
"""

from dataclasses import dataclass, field


@dataclass
class Event:
    frm: str = ""       # originating actor id ("" = system/user)
    to: str = ""        # target actor id ("" = broadcast)
    corr: str = ""      # correlation id (set by the bus on request)
    turn: str = ""      # turn this event belongs to


@dataclass
class TurnStarted(Event):
    pass


@dataclass
class TextDelta(Event):
    text: str = ""


@dataclass
class ToolCall(Event):
    id: str = ""
    name: str = ""
    args: dict = field(default_factory=dict)


@dataclass
class ToolResult(Event):
    id: str = ""
    name: str = ""
    observation: str = ""


@dataclass
class LLMCall(Event):
    model: str = ""
    prompt_tokens: int = 0
    completion_tokens: int = 0
    total_tokens: int = 0


@dataclass
class TurnDone(Event):
    final_text: str = ""


@dataclass
class TurnCancelled(Event):
    note: str = "cancelled by user"


@dataclass
class Spawn(Event):
    spec: str = ""      # subagent name
    task: str = ""      # task description for the child


@dataclass
class SpawnResult(Event):
    final_text: str = ""


@dataclass
class ApprovalNeeded(Event):
    tool: str = ""
    args: dict = field(default_factory=dict)


@dataclass
class PermissionRequest(Event):
    tool: str = ""
    args: dict = field(default_factory=dict)


@dataclass
class ApprovalDecision(Event):
    allowed: bool = False
    feedback: str = ""


@dataclass
class Cancel(Event):
    pass
