"""Message-history storage interface for the agent.

`InMemoryStore` is the default and reproduces today's behavior exactly (a plain
in-process list, no persistence). `RaftMessageStore` double-writes each message
into a nanoraft KV cluster under ``session/{id}/{seq:08d}`` (zero-padded so a
prefix Scan returns them in order), so a fresh nanoCoder attached to the same
``--session`` can Scan the committed history back and resume the conversation.
"""

import itertools


class InMemoryStore:
    """Default store: holds messages in a list. append/load with no durability."""

    def __init__(self):
        self._msgs = []

    def append(self, msg):
        self._msgs.append(msg)

    def load(self):
        return list(self._msgs)


class RaftMessageStore:
    """Durable store backed by a RaftKvClient. Each message is a Put under
    ``session/{id}/{seq:08d}``; load() Scans the prefix (already key-sorted) and
    unpacks. Numbering resumes after whatever the cluster already holds, so a
    re-attach appends rather than clobbering the restored history."""

    def __init__(self, kv, session_id):
        self.kv = kv
        self.sid = session_id
        existing = kv.scan(self._prefix())
        self._seq = itertools.count(len(existing))   # resume after existing messages

    def _prefix(self):
        return f"session/{self.sid}/"

    def append(self, msg):
        i = next(self._seq)
        import msgpack  # 懒导入：只有 --raft 这条路才需要它（依赖见根目录 pyproject.toml）
        self.kv.put(f"{self._prefix()}{i:08d}", msgpack.packb(msg, use_bin_type=True))

    def load(self):
        rows = self.kv.scan(self._prefix())          # sorted by key = message order
        import msgpack  # 懒导入，同 append()
        return [msgpack.unpackb(v, raw=False) for _, v in rows]
