"""PermissionHarness: the stateful session-allow gate a frontend holds.

The frontend asks the user whether a dangerous tool may run; the harness
remembers any "allow all during this session" choices so later calls in the
same scope skip the prompt. Policy and decision-reading live in `policy`; this
adds only the per-session state on top of them.
"""

from . import policy


class PermissionHarness:
    def __init__(self, auto_approve_all: bool = False):
        self.auto_approve_all = auto_approve_all
        self._allowed_scopes = set()

    def needs_approval(self, name: str) -> bool:
        return policy.needs_approval(name)

    def session_scope(self, name: str) -> str:
        return policy.session_scope(name)

    def is_session_allowed(self, name: str) -> bool:
        return self.auto_approve_all or policy.session_scope(name) in self._allowed_scopes

    def allow_session(self, name: str):
        """Remember an "allow all" choice. Allowing edits is scoped to edits;
        allowing a command (or any other tool) flips global auto-approve."""
        if policy.session_scope(name) == "edits":
            self._allowed_scopes.add("edits")
        else:
            self.auto_approve_all = True
