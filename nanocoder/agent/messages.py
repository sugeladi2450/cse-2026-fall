"""OpenAI-shaped message helpers for the agent loop."""

import json


def parse_tool_call(tc: dict):
    fn = tc.get("function", {})
    name = fn.get("name", "")
    raw_args = fn.get("arguments") or "{}"
    try:
        args = json.loads(raw_args)
    except (TypeError, json.JSONDecodeError) as exc:
        return name, {}, f"error: invalid tool arguments JSON for {name}: {exc}"
    if not isinstance(args, dict):
        return name, {}, f"error: invalid tool arguments JSON for {name}: expected object"
    return name, args, None


def tool_observation(tool_call_id: str, observation: str) -> dict:
    return {"role": "tool", "tool_call_id": tool_call_id, "content": observation}
