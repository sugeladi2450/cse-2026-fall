"""Tool registry: holds the OpenAI-style schemas the LLM sees and the
name -> callable dispatch table the agent loop uses.

A tool is {name, description, parameters schema, fn(**args) -> str}. Dispatch
catches tool errors and returns them as observation strings so the agent loop
never crashes on a bad tool call (see SPEC error handling).
"""

import inspect

from .fs_tools import FsTools
from .schema import ToolSpec, build_tool_specs
from .shell_tools import ShellTools


class Registry:
    def __init__(self, tools):
        self._specs = [tool if isinstance(tool, ToolSpec) else ToolSpec(*tool) for tool in tools]
        self._schemas = [tool.schema for tool in self._specs]
        self._fns = {tool.schema["function"]["name"]: tool.fn for tool in self._specs}
        # Tools whose callable accepts a `cancel` kwarg get the turn's token so
        # long-running ones (bash) can abort; the rest are called unchanged.
        self._cancel_aware = {
            name for name, fn in self._fns.items()
            if "cancel" in inspect.signature(fn).parameters
        }

    def schemas(self):
        return self._schemas

    def subset(self, allowed):
        """A new Registry with only the tools named in `allowed`, never `task`
        (subagents cannot spawn subagents)."""
        keep = [s for s in self._specs
                if s.schema["function"]["name"] in allowed
                and s.schema["function"]["name"] != "task"]
        return Registry(keep)

    def dispatch(self, name: str, args: dict, cancel=None) -> str:
        fn = self._fns.get(name)
        if fn is None:
            return f"error: unknown tool {name!r}"
        call_args = dict(args)
        if cancel is not None and name in self._cancel_aware:
            call_args["cancel"] = cancel
        try:
            return str(fn(**call_args))
        except Exception as e:
            return f"error: {type(e).__name__}: {e}"


def build_registry(workspace, bash_timeout: float = 30.0, skills=None, with_task=False) -> Registry:
    """Build the tool registry. When `skills` (a Skills facade) is given, a
    load_skill tool is registered alongside the file/shell tools. When
    `with_task` is set, a `task` tool spec is appended (its fn is a stub;
    the Agent intercepts `task` before dispatch and spawns via the bus)."""
    fs = FsTools(workspace)
    sh = ShellTools(workspace, timeout=bash_timeout)
    specs = build_tool_specs(fs, sh)
    if skills is not None:
        from .schema import load_skill_spec
        specs = specs + [load_skill_spec(skills)]
    if with_task:
        from .schema import task_spec
        specs = specs + [task_spec()]
    return Registry(specs)
