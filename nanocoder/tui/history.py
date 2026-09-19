"""Session input history with shell-style ↑/↓ navigation.

Kept independent of prompt_toolkit so the navigation logic is testable on its
own. `older()` / `newer()` return the text to show, or None when there is
nothing to change.
"""


class InputHistory:
    def __init__(self):
        self._items = []
        self._idx = None  # None means editing a fresh line below the newest entry

    def add(self, text: str):
        """Record a submitted line and reset navigation to the fresh line."""
        if text.strip() and (not self._items or self._items[-1] != text):
            self._items.append(text)
        self._idx = None

    def older(self):
        """Step toward older entries (↑). Returns text, or None if no change."""
        if not self._items:
            return None
        if self._idx is None:
            self._idx = len(self._items)
        if self._idx > 0:
            self._idx -= 1
        return self._items[self._idx]

    def newer(self):
        """Step toward newer entries (↓). Returns text, "" at the fresh line, or None."""
        if self._idx is None:
            return None
        self._idx += 1
        if self._idx >= len(self._items):
            self._idx = None
            return ""
        return self._items[self._idx]
