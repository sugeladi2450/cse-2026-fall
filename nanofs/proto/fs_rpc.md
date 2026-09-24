# nanofs RPC protocol

Everything is addressed by **file handle** `(inode_id:u32, gen:u32)`; the server
does not understand paths. Encoding is msgpack. `gen` is a generation number
that exposes old handles whose inode number was reused after a delete (the NFS
file-handle idea). The root handle is `(1, 1)`.
Every method returns an envelope `[err:int, result]`: `err==0` means success and
`result` is the return value; `err!=0` means failure and `result` is the
**default value of the success type** (the client only looks at err and ignores
it — this keeps the msgpack type of `result` fixed, so strongly typed
deserialization works).

Error codes (errno-style, matching the C++ `Error` enum):
`0` ok · `2` ENOENT(NotFound) · `5` EIO(IoError) · `17` EEXIST · `20` ENOTDIR(NotDir)
· `21` EISDIR(IsDir) · `22` EINVAL(Invalid) · `28` ENOSPC(NoSpace) · `39` ENOTEMPTY(NotEmpty)
· `70` ESTALE(Stale) — the inode is unallocated/out of range, or its generation differs.

Each server handler first checks allocation and generation with `resolve_handle`
(invalid handle → `70`), then forwards to the FS layer.
Methods that return a handle (lookup/create/mkdir) send the result inode's
current gen back with it.

| method | args | result (on success) | errors | idempotence |
|---|---|---|---|---|
| `fs_lookup`  | `parent_id:u32, parent_gen:u32, name:str` | `[inode_id:u32, gen:u32]` | ENOENT / ENOTDIR / ESTALE | read-only |
| `fs_getattr` | `inode_id:u32, gen:u32` | `[inode_id:u32, is_dir:bool, size:u32]` | ESTALE | read-only |
| `fs_readdir` | `inode_id:u32, gen:u32` | `[[name:str, inode_id:u32], ...]` | ENOTDIR / ESTALE | read-only |
| `fs_read`    | `inode_id:u32, gen:u32, off:u32, len:u32` | `bytes:str` | EISDIR / EINVAL / ESTALE | read-only |
| `fs_write`   | `inode_id:u32, gen:u32, off:u32, bytes:str` | `written:u32` | EISDIR / EINVAL / ENOSPC / ESTALE | resending the same range gives the same content |
| `fs_create`  | `parent_id:u32, parent_gen:u32, name:str` | `[inode_id:u32, gen:u32]` | ENOTDIR / EINVAL / ENOSPC / ESTALE | name already exists → returns the existing handle |
| `fs_mkdir`   | `parent_id:u32, parent_gen:u32, name:str` | `[inode_id:u32, gen:u32]` | ENOTDIR / EINVAL / ENOSPC / ESTALE | name already exists → returns the existing handle |
| `fs_unlink`  | `parent_id:u32, parent_gen:u32, name:str` | `ok:bool`(true) | ENOTDIR / EISDIR / ESTALE | name missing → still returns ok |
| `fs_rmdir`   | `parent_id:u32, parent_gen:u32, name:str` | `ok:bool`(true) | ENOTDIR / ENOTEMPTY / ESTALE | name missing → still returns ok |
| `fs_setattr` | `inode_id:u32, gen:u32, size:u32` | `[inode_id:u32, is_dir:bool, size:u32]` | EISDIR / EINVAL / ENOSPC / ESTALE | idempotent |

The errors column lists common errors, not every possible failure. Type errors,
invalid arguments and storage errors follow the [Lab1A contracts](../../docs/lab1/lab1a.md#error-handling).
For example, unlinking a directory returns IsDir and removing a file with rmdir
returns NotDir. Application errors are encoded in the reply and are not retried.

On a transport exception or timeout the client retries a bounded number of times.
The server may already have executed a request whose reply was lost. The
idempotence column describes repeated requests without intervening conflicting
operations. These retries do not guarantee delivery or exactly-once execution;
there is no request-ID/reply cache in this lab.
