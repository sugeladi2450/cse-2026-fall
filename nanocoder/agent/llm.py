"""LLM backends.

Both backends speak the OpenAI chat-completions message shape so the agent
loop treats them identically:

  - assistant message with `tool_calls` -> keep going
  - assistant message with text `content` -> done

MockBackend replays a fixed script (deterministic, no network) for tests and
grading. OpenAIBackend calls a real OpenAI-compatible /chat/completions API.
"""

import json
import time
import urllib.request

from ..metrics import LLMMetrics, from_usage


class LLMBackend:
    last_metrics = None  # set to an LLMMetrics after each step()/stream()

    def step(self, messages: list, tools: list) -> dict:
        raise NotImplementedError


def _to_tool_calls(specs: list) -> list:
    """Turn the ergonomic script form into OpenAI-shaped tool_calls."""
    calls = []
    for i, spec in enumerate(specs):
        arguments = spec["arguments"]
        if not isinstance(arguments, str):
            arguments = json.dumps(arguments)
        calls.append(
            {
                "id": f"call_{i}",
                "type": "function",
                "function": {"name": spec["name"], "arguments": arguments},
            }
        )
    return calls


class MockBackend(LLMBackend):
    """Replays a scripted list of turns, one per step() call.

    Each turn is either:
      {"tool_calls": [{"name": ..., "arguments": {...}}, ...]}
      {"content": "final text"}
    """

    def __init__(self, script: list):
        self._script = list(script)
        self._i = 0

    @classmethod
    def from_file(cls, path: str) -> "MockBackend":
        with open(path) as f:
            return cls(json.load(f))

    def step(self, messages: list, tools: list) -> dict:
        self.last_metrics = LLMMetrics(model="mock")
        if self._i >= len(self._script):
            return {"role": "assistant", "content": "(mock script exhausted)"}
        turn = self._script[self._i]
        self._i += 1
        if "tool_calls" in turn:
            return {
                "role": "assistant",
                "content": None,
                "tool_calls": _to_tool_calls(turn["tool_calls"]),
            }
        return {"role": "assistant", "content": turn.get("content", "")}

    def stream(self, messages: list, tools: list):
        """Streaming twin of step(): yield ("delta", text) then return the message.

        A text turn yields its content as a single delta; a tool_calls turn
        yields nothing. The returned message is identical to step()'s.
        """
        msg = self.step(messages, tools)
        if not msg.get("tool_calls") and msg.get("content"):
            yield ("delta", msg["content"])
        return msg


def _http_transport(base_url: str, api_key: str, timeout: float = 120.0):
    """Default transport: POST payload to <base_url>/chat/completions.

    `timeout` bounds the request in seconds so a hung remote cannot hang the
    agent forever."""
    url = base_url.rstrip("/") + "/chat/completions"

    def transport(payload: dict) -> dict:
        body = json.dumps(payload).encode()
        req = urllib.request.Request(url, data=body, method="POST")
        req.add_header("Content-Type", "application/json")
        if api_key:
            req.add_header("Authorization", f"Bearer {api_key}")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())

    return transport


def _http_line_transport(base_url: str, api_key: str, timeout: float = 120.0):
    """Streaming transport: POST a stream payload, yield decoded SSE lines.

    `timeout` bounds the connection so a hung remote cannot hang the agent."""
    url = base_url.rstrip("/") + "/chat/completions"

    def line_transport(payload: dict):
        body = json.dumps(payload).encode()
        req = urllib.request.Request(url, data=body, method="POST")
        req.add_header("Content-Type", "application/json")
        if api_key:
            req.add_header("Authorization", f"Bearer {api_key}")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            for raw in resp:
                yield raw.decode("utf-8").rstrip("\n")

    return line_transport


def _merge_tool_call_delta(acc: dict, delta: dict):
    """Accumulate a streamed tool_call delta into `acc` keyed by index."""
    i = delta.get("index", 0)
    # OpenAI streams function name/arguments in fragments; keep one slot per call.
    slot = acc.setdefault(i, {"id": None, "type": "function",
                              "function": {"name": "", "arguments": ""}})
    if delta.get("id"):
        slot["id"] = delta["id"]
    if delta.get("type"):
        slot["type"] = delta["type"]
    fn = delta.get("function", {})
    if fn.get("name"):
        slot["function"]["name"] += fn["name"]
    if fn.get("arguments"):
        slot["function"]["arguments"] += fn["arguments"]


class OpenAIBackend(LLMBackend):
    """Calls an OpenAI-compatible /chat/completions endpoint with tool calling.

    `transport` (for step) and `line_transport` (for stream) are injectable for
    testing; by default they POST over HTTP.
    """

    def __init__(self, model, base_url, api_key=None, timeout=120.0,
                 transport=None, line_transport=None):
        self.model = model
        self.transport = transport or _http_transport(base_url, api_key, timeout)
        self.line_transport = line_transport or _http_line_transport(base_url, api_key, timeout)

    def step(self, messages: list, tools: list) -> dict:
        payload = {"model": self.model, "messages": messages}
        if tools:
            payload["tools"] = tools
        start = time.monotonic()
        data = self.transport(payload)
        self.last_metrics = from_usage(data.get("usage"), self.model,
                                       wall_s=time.monotonic() - start)
        return data["choices"][0]["message"]

    def stream(self, messages: list, tools: list):
        """Stream tokens: yield ("delta", text) per chunk, return final message.

        Parses OpenAI SSE chunks, forwarding text deltas as they arrive and
        accumulating any tool_call deltas into a single assembled message.
        """
        payload = {"model": self.model, "messages": messages, "stream": True,
                   "stream_options": {"include_usage": True}}
        if tools:
            payload["tools"] = tools

        content = ""
        tool_acc = {}
        error_text = None
        usage = None
        start = time.monotonic()
        ttft = None
        for line in self.line_transport(payload):
            if not line or not line.startswith("data:"):
                continue  # skip keep-alive comments and blank lines
            data = line[len("data:"):].strip()
            if data == "[DONE]":
                break
            try:
                obj = json.loads(data)
            except json.JSONDecodeError:
                continue  # tolerate malformed frames rather than crash
            if isinstance(obj.get("error"), dict):
                error_text = obj["error"].get("message", "unknown error")
                break
            if obj.get("usage"):
                usage = obj["usage"]  # final include_usage frame
            choices = obj.get("choices") or []
            if not choices:
                continue  # usage-only / heartbeat frames carry no choice
            delta = choices[0].get("delta", {})
            if delta.get("content"):
                if ttft is None:
                    ttft = time.monotonic() - start
                content += delta["content"]
                yield ("delta", delta["content"])
            for tc in delta.get("tool_calls", []):
                _merge_tool_call_delta(tool_acc, tc)

        self.last_metrics = from_usage(usage, self.model,
                                       wall_s=time.monotonic() - start, ttft_s=ttft)
        if error_text is not None:
            return {"role": "assistant", "content": f"error: {error_text}"}
        msg = {"role": "assistant", "content": content or None}
        if tool_acc:
            msg["tool_calls"] = [tool_acc[i] for i in sorted(tool_acc)]
        return msg
