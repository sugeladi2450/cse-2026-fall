"""Slash command handling shared by TUI frontends."""

from dataclasses import dataclass
from enum import Enum, auto

from prompt_toolkit.completion import Completer, Completion


class CommandAction(Enum):
    NOT_COMMAND = auto()
    CONTINUE = auto()
    EXIT = auto()


@dataclass(frozen=True)
class SlashCommand:
    name: str
    description: str
    action: CommandAction


SLASH_COMMANDS = (
    SlashCommand("/help", "List commands", CommandAction.CONTINUE),
    SlashCommand("/tool", "List available tools", CommandAction.CONTINUE),
    SlashCommand("/skill", "List skills, or /skill <name> to load", CommandAction.CONTINUE),
    SlashCommand("/context", "Estimate next-call token size", CommandAction.CONTINUE),
    SlashCommand("/clear", "Reset conversation history", CommandAction.CONTINUE),
    SlashCommand("/log", "Show the session log directory", CommandAction.CONTINUE),
    SlashCommand("/plan", "Toggle read-only plan mode (/plan apply to exit)", CommandAction.CONTINUE),
    SlashCommand("/exit", "Exit nanoCoder", CommandAction.EXIT),
)


def handle_command(line: str, ui, agent=None) -> CommandAction:
    """Handle slash-prefixed input without sending it to the agent.

    `agent` gives context commands access to history/tools/skills/logger; it may
    be None for context-free commands like /help. Returns NOT_COMMAND for plain
    input, EXIT for /exit, and CONTINUE otherwise (with status output on ui)."""
    if not line.startswith("/"):
        return CommandAction.NOT_COMMAND
    name, _, rest = line.partition(" ")
    rest = rest.strip()

    if name == "/exit":
        return CommandAction.EXIT
    if name == "/help":
        _status(ui, "commands:\n" + "\n".join(f"  {c.name} — {c.description}"
                                               for c in SLASH_COMMANDS))
    elif name == "/tool" and agent is not None:
        names = [s["function"]["name"] for s in agent.registry.schemas()]
        _status(ui, "tools: " + ", ".join(names))
    elif name == "/skill" and agent is not None:
        if rest:
            _status(ui, agent.registry.dispatch("load_skill", {"name": rest}))
        else:
            _status(ui, agent.skills_catalog or "(no skills discovered)")
    elif name == "/context" and agent is not None:
        chars = sum(len(str(m.get("content") or "")) for m in agent.messages)
        _status(ui, f"~{chars} chars / ~{chars // 4} tokens in history")
    elif name == "/clear" and agent is not None:
        agent.messages = []
        if hasattr(ui, "clear_transcript"):
            ui.clear_transcript()  # also wipe the visible transcript window
        _status(ui, "history cleared")
    elif name == "/log" and agent is not None:
        log_dir = getattr(agent.logger, "dir", None)
        _status(ui, f"log: {log_dir}" if log_dir else "no session log")
    elif name == "/plan" and agent is not None:
        agent.plan_mode = rest not in {"apply", "off"}
        _status(ui, "plan mode ON (read-only)" if agent.plan_mode
                else "plan mode OFF")
    else:
        _status(ui, f"unsupported command: {line}")
    return CommandAction.CONTINUE


class SlashCommandCompleter(Completer):
    """prompt_toolkit completer for supported slash commands."""

    def get_completions(self, document, complete_event):
        text = document.text_before_cursor
        if not text.startswith("/") or any(char.isspace() for char in text):
            return

        for command in SLASH_COMMANDS:
            if command.name.startswith(text):
                yield Completion(
                    command.name,
                    start_position=-len(text),
                    display=command.name,
                    display_meta=command.description,
                )


def _status(ui, text: str):
    if hasattr(ui, "on_status"):
        ui.on_status(text)
    else:
        ui.on_error(ValueError(text))
