"""A tiny msgpack-rpc client for a nanoraftd KV cluster.

`MsgpackRpcClient` speaks rpclib's msgpack-rpc framing over a socket (request
``[0, msgid, method, params]`` / response ``[1, msgid, error, result]``).
`RaftKvClient` layers leader-redirect, dead-node failover, and exactly-once
`(client_id, req_seq)` numbering on top, exposing put/get/scan.

Wire contract (confirmed against a live nanoraftd):
  * responses are read with ``raw=True`` — a KV value is opaque bytes, not text.
  * a KV reply is ``[ok: bool, leader_hint: int, result: bytes]``; ok=False means
    "not the leader, retry leader_hint" (or another node if the hint is -1).
  * ``kv_put`` params are ``(key, value, client_id, req_seq)``; reads take only the
    key/prefix. ``kv_scan``'s result is itself a msgpack blob of ``[[key, value]…]``.
"""

import itertools
import socket
import uuid

import msgpack

_MSGIDS = itertools.count(1)


class MsgpackRpcClient:
    """Minimal msgpack-rpc client for an rpclib server. One TCP connection, lazily
    opened; `call` blocks up to `timeout` for the reply and raises on any transport
    error (so callers can fail over to another node)."""

    def __init__(self, host, port, timeout=1.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock = None

    def _connect(self):
        self._sock = socket.create_connection((self.host, self.port), self.timeout)
        self._sock.settimeout(self.timeout)

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def call(self, method, *params):
        """Send one request and return the server's `result`. Raises on transport
        error or an rpc-level error reply. Byte-preserving (raw=True)."""
        if self._sock is None:
            self._connect()
        msgid = next(_MSGIDS)
        self._sock.sendall(msgpack.packb([0, msgid, method, list(params)], use_bin_type=True))
        unpacker = msgpack.Unpacker(raw=True)
        while True:
            data = self._sock.recv(65536)
            if not data:
                raise ConnectionError("connection closed by server")
            unpacker.feed(data)
            for msg in unpacker:
                _typ, _id, err, result = msg
                if err is not None:
                    raise RuntimeError(err)
                return result


class RaftKvClient:
    """KV client over a nanoraftd cluster: finds the leader (following leader_hint),
    fails over dead nodes, and stamps mutations with a per-process client_id + a
    monotonic req_seq so a retried write is applied exactly once."""

    def __init__(self, nodes, client_id=None):
        self.nodes = list(nodes)                 # [(host, port), ...], id-ordered
        self.client_id = client_id or uuid.uuid4().hex
        self._seq = itertools.count(1)
        self._leader = 0                         # index into nodes; just a hint
        self._clients = {}

    def _client(self, i):
        if i not in self._clients:
            self._clients[i] = MsgpackRpcClient(*self.nodes[i])
        return self._clients[i]

    def _drop(self, i):
        c = self._clients.pop(i, None)
        if c is not None:
            c.close()

    def _request(self, method, params, seq=None):
        """Drive `method` to the current leader, retrying redirects/dead nodes.
        `seq` (with client_id) is appended for mutations; None for reads. Returns
        the reply's `result` bytes."""
        call_params = list(params)
        if seq is not None:
            call_params += [self.client_id, seq]
        i = self._leader
        for _ in range(len(self.nodes) * 4):
            try:
                ok, hint, result = self._client(i).call(method, *call_params)
                if ok:
                    self._leader = i
                    return result
                i = hint if (hint is not None and 0 <= hint < len(self.nodes)) \
                    else (i + 1) % len(self.nodes)
            except Exception:
                self._drop(i)                    # reconnect on the next attempt
                i = (i + 1) % len(self.nodes)
        raise RuntimeError("no reachable leader")

    def put(self, key, value):
        """Append/overwrite key=value (value: bytes). Exactly-once across retries."""
        self._request("kv_put", [key, value], seq=next(self._seq))

    def get(self, key):
        """Return the value bytes at key (b'' if absent)."""
        return self._request("kv_get", [key])

    def scan(self, prefix):
        """Return [(key_str, value_bytes), ...] for keys under `prefix`, sorted."""
        blob = self._request("kv_scan", [prefix])
        rows = msgpack.unpackb(blob, raw=True) if blob else []
        return [(k.decode(), v) for k, v in rows]
