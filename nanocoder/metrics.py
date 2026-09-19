"""Per-call and aggregate LLM usage metrics.

A backend sets `.last_metrics` after each call; the agent merges them into a
running total for the session footer and the session log.
"""

from dataclasses import dataclass, replace


@dataclass
class LLMMetrics:
    prompt_tokens: int = 0
    completion_tokens: int = 0
    total_tokens: int = 0
    wall_s: float = 0.0
    ttft_s: float = None
    model: str = ""

    def merge(self, other: "LLMMetrics") -> "LLMMetrics":
        """Return a copy with token counts and wall time summed; ttft/model keep
        the first non-empty value (self wins)."""
        return replace(
            self,
            prompt_tokens=self.prompt_tokens + other.prompt_tokens,
            completion_tokens=self.completion_tokens + other.completion_tokens,
            total_tokens=self.total_tokens + other.total_tokens,
            wall_s=self.wall_s + other.wall_s,
            ttft_s=self.ttft_s if self.ttft_s is not None else other.ttft_s,
            model=self.model or other.model,
        )


def from_usage(usage: dict, model: str, wall_s: float = 0.0, ttft_s: float = None) -> LLMMetrics:
    """Build metrics from an OpenAI `usage` dict (missing fields default to 0)."""
    usage = usage or {}
    return LLMMetrics(
        prompt_tokens=usage.get("prompt_tokens", 0),
        completion_tokens=usage.get("completion_tokens", 0),
        total_tokens=usage.get("total_tokens", 0),
        wall_s=wall_s,
        ttft_s=ttft_s,
        model=model,
    )
