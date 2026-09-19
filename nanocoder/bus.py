"""Async event bus: fan-out publish/subscribe + correlation-id request/reply.

The single coordination substrate. Actors (agents), the TUI, the logger and
metrics attach here. publish() broadcasts observation events; request()/reply()
carry operations that need an answer, keyed by a correlation id. Each turn's
in-flight requests are tracked so cancel_turn() can fail them promptly.
"""

import asyncio
import itertools


class RequestCancelled(Exception):
    """A pending request was failed because its turn received a Cancel."""


class RequestTimeout(Exception):
    """A pending request was not answered within its timeout."""


class Bus:
    def __init__(self):
        self._subs = []                 # list[(predicate, handler)]
        self._pending = {}              # corr -> (Future, turn)
        self._corr = itertools.count(1)

    def subscribe(self, predicate, handler):
        """Register handler(event) for events where predicate(event) is true.
        Handlers run synchronously inside publish(); keep them non-blocking
        (schedule asyncio tasks for async work)."""
        self._subs.append((predicate, handler))

    def unsubscribe(self, handler):
        """Remove every subscription registered with `handler`."""
        self._subs = [(p, h) for (p, h) in self._subs if h != handler]

    def publish(self, event):
        """Deliver event to every matching subscriber, in registration order."""
        for predicate, handler in list(self._subs):
            if predicate(event):
                handler(event)

    async def request(self, event, timeout=None):
        """Publish event with a fresh corr and await its reply Event.

        Raises RequestTimeout if no reply arrives within `timeout` seconds.
        Raises RequestCancelled if event.turn is cancelled via cancel_turn()
        while this request is in flight."""
        corr = str(next(self._corr))
        event.corr = corr
        loop = asyncio.get_running_loop()
        future = loop.create_future()
        self._pending[corr] = (future, event.turn)
        self.publish(event)
        try:
            if timeout is not None:
                return await asyncio.wait_for(future, timeout)
            return await future
        except asyncio.TimeoutError:
            raise RequestTimeout()
        finally:
            self._pending.pop(corr, None)

    def cancel_turn(self, turn):
        """Fail every in-flight request belonging to `turn` with RequestCancelled."""
        for corr, (future, req_turn) in list(self._pending.items()):
            if req_turn == turn and not future.done():
                future.set_exception(RequestCancelled())

    def reply(self, request_event, response_event):
        """Answer a request: resolve the future waiting on request_event.corr."""
        response_event.corr = request_event.corr
        entry = self._pending.get(request_event.corr)
        if entry is not None:
            future, _ = entry
            if not future.done():
                future.set_result(response_event)
