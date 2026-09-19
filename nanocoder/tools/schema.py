"""Tool schema construction."""

from collections.abc import Callable
from dataclasses import dataclass


@dataclass(frozen=True)
class ToolSpec:
    schema: dict
    fn: Callable


def schema(name, description, properties, required):
    return {
        "type": "function",
        "function": {
            "name": name,
            "description": description,
            "parameters": {
                "type": "object",
                "properties": properties,
                "required": required,
            },
        },
    }


def build_tool_specs(fs, sh):
    return [
        ToolSpec(
            schema(
                "read_file",
                "Read a workspace file as line-numbered text. Large files are "
                "windowed: pass offset/limit (1-indexed lines) to page through. "
                "Line numbers are for reference only and not part of file content.",
                {
                    "path": {"type": "string", "description": "Path relative to workspace."},
                    "offset": {"type": "integer", "description": "1-indexed start line (optional)."},
                    "limit": {"type": "integer", "description": "Max lines to return (optional)."},
                },
                ["path"],
            ),
            fs.read_file,
        ),
        ToolSpec(
            schema(
                "write_file",
                "Write content to a file, creating parent directories as needed.",
                {
                    "path": {"type": "string", "description": "Path relative to workspace."},
                    "content": {"type": "string", "description": "Full file content."},
                },
                ["path", "content"],
            ),
            fs.write_file,
        ),
        ToolSpec(
            schema(
                "edit_file",
                "Replace a unique substring in a file with new text.",
                {
                    "path": {"type": "string", "description": "Path relative to workspace."},
                    "old": {"type": "string", "description": "Exact substring to replace (must be unique)."},
                    "new": {"type": "string", "description": "Replacement text."},
                },
                ["path", "old", "new"],
            ),
            fs.edit_file,
        ),
        ToolSpec(
            schema(
                "bash",
                "Run a bash command. Its working directory starts at the "
                "workspace, but bash is NOT sandboxed to it (it can cd out); "
                "very large output is truncated.",
                {
                    "command": {"type": "string", "description": "Shell command to run."},
                    "description": {
                        "type": "string",
                        "description": "One short sentence describing what the command does, shown to the user when approving.",
                    },
                },
                ["command"],
            ),
            sh.bash,
        ),
    ]


def task_spec():
    """ToolSpec for the `task` tool. The Agent intercepts `task` before
    dispatch (it spawns a subagent via the bus), so fn is a never-called stub."""
    def _stub(**kwargs):
        return "error: task is handled by the agent, not dispatched"
    return ToolSpec(
        schema(
            "task",
            "Delegate a self-contained subtask to a subagent that works "
            "autonomously and returns a summary. subagent_type picks the agent "
            "(e.g. 'general'); description is the task.",
            {
                "subagent_type": {"type": "string",
                                  "description": "Name of the subagent to run."},
                "description": {"type": "string",
                                "description": "The subtask for the subagent."},
            },
            ["subagent_type", "description"],
        ),
        _stub,
    )


def load_skill_spec(skills):
    """ToolSpec for load_skill, backed by a Skills facade (fn: name -> body)."""
    return ToolSpec(
        schema(
            "load_skill",
            "Load the full body of a named skill from the catalog.",
            {"name": {"type": "string", "description": "Skill name from the catalog."}},
            ["name"],
        ),
        skills.load,
    )
