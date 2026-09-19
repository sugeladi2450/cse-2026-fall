"""Thread-safe one-shot cancel signal.

The UI/main thread calls cancel(); the agent worker thread polls cancelled at
step boundaries. Wrapping threading.Event keeps the cross-thread hand-off safe
without any locking in the agent loop.
"""

import threading


class CancelToken:
    def __init__(self):
        self._event = threading.Event()

    def cancel(self):
        """Request cancellation. Idempotent; safe from any thread."""
        self._event.set()

    @property
    def cancelled(self) -> bool:
        return self._event.is_set()
