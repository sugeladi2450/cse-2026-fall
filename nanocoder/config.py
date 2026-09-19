"""Config file discovery + load.

A nanoCoder config is a JSON object with optional keys:

  {"model": "...", "base_url": "...", "api_key": "...",
   "raft": "h0:p0,h1:p1,h2:p2"}

`raft` is the default nanoraftd node list for durable sessions; a `--raft`
flag overrides it, and it only takes effect together with `--session`.

Two locations are searched and merged, project on top of user:

  1. ~/.config/nanocoder/config.json   (user-level)
  2. ./nanocoder.config.json           (project-level, CWD)

`load_config()` with no path merges those two (missing files are skipped).
`load_config(path)` loads exactly that file instead.
"""

import json
import os

USER_CONFIG = os.path.expanduser("~/.config/nanocoder/config.json")
PROJECT_CONFIG = "nanocoder.config.json"


def _read(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def load_config(path: str = None) -> dict:
    if path:
        return _read(path)
    config = {}
    for p in (USER_CONFIG, PROJECT_CONFIG):
        if os.path.exists(p):
            config.update(_read(p))
    return config
