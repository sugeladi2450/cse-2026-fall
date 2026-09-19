"""Local skill bundles: SKILL.md files discovered under project/user roots.

A skill is a directory with a SKILL.md whose YAML-ish frontmatter carries a name
and description. The compact catalog (name + description) is injected into the
system prompt; full bodies are pulled on demand via the load_skill tool. No
pinning, no inline $skill syntax (minimal altitude).
"""

import os
from pathlib import Path


def _parse_frontmatter(text: str) -> dict:
    """Read the leading --- fenced `key: value` block; empty dict if absent."""
    meta = {}
    if not text.startswith("---"):
        return meta
    end = text.find("\n---", 3)
    if end == -1:
        return meta
    for line in text[3:end].splitlines():
        if ":" in line:
            key, _, value = line.partition(":")
            meta[key.strip()] = value.strip()
    return meta


class Skill:
    def __init__(self, name: str, description: str, path: Path):
        self.name = name
        self.description = description
        self.path = path

    def body(self) -> str:
        return self.path.read_text()


def discover_skills(roots) -> list:
    """Return Skills found as <root>/*/SKILL.md across the given roots.

    Earlier roots win on name collisions (project shadows user)."""
    found = {}
    for root in roots:
        root = Path(root).expanduser()
        if not root.is_dir():
            continue
        for skill_md in sorted(root.glob("*/SKILL.md")):
            meta = _parse_frontmatter(skill_md.read_text())
            name = meta.get("name") or skill_md.parent.name
            found.setdefault(name, Skill(name, meta.get("description", ""), skill_md))
    return list(found.values())


DEFAULT_ROOTS = [".nanocoder/skills", os.path.expanduser("~/.nanocoder/skills")]


class Skills:
    def __init__(self, roots=None):
        self._skills = {s.name: s for s in discover_skills(roots or DEFAULT_ROOTS)}

    def catalog_text(self) -> str:
        """One `- name: description` line per skill (empty string if none)."""
        return "\n".join(f"- {s.name}: {s.description}" for s in self._skills.values())

    def load(self, name: str) -> str:
        """Full SKILL.md body for `name`, or an error observation string."""
        skill = self._skills.get(name)
        if skill is None:
            return f"error: unknown skill {name!r}"
        return skill.body()
