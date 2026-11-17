# OpenMacFUSE design

OpenMacFUSE lets a FUSE 2.x high-level file system run on macOS without a kernel extension and without any separately installed component. The library (`libnfuse.a`) starts an NFSv4.0 server inside the file system's own process and asks the NFS client that ships with macOS (`/sbin/mount_nfs`) to mount it. It is one of a small number of open-source macOS FUSE implementations that need no kernel extension.

```
application
   |  POSIX calls
macOS NFS client (in the kernel, ships with macOS)
   |  ONC RPC over a local unix socket or 127.0.0.1 TCP
+--+---------------- your process --------------------------+
| libnfuse.a                                                |
|   server.c        connections, RPC record framing, pool   |
|   rpc.c xdr.c     ONC RPC (RFC 5531), XDR (RFC 4506)      |
|   nfs4_ops.c      COMPOUND dispatch (RFC 7530/7531)       |
|   nfs4_state.c    clients, open/lock state, replay caches |
|   backend_fuse.c  inode <-> path, snapshots, name index   |
|   adfile.c        virtual `._` files <-> xattr callbacks  |
|   names.c         NFC normalisation, case folding         |
|   fuse_lib.c      fuse_mount / fuse_new / fuse_loop       |
|   fuse_main.c fuse_opt_parse.c   fuse_main, option parser |
|   watchdog.c devwatch.c   guard process, device watcher   |
|   mounter.c       posix_spawn mount_nfs, unmount          |
|        |                                                  |
|   your fuse_operations callbacks                          |
+-----------------------------------------------------------+
```

Identifiers keep the prefix `nfuse` (`NFUSE_*` environment variables, `nfuse_*` functions and mount options).

## Sources

Implemented from public specifications: RFC 7530 and 7531 (NFSv4.0), RFC 5531 (ONC RPC), RFC 4506 (XDR), RFC 1740 (AppleDouble header), the public description of the FUSE 2.x high-level API, and observation of the requests the macOS NFS client sends to this server (server-side logs). No other NFS server or FUSE library source was used, and no third-party binary was disassembled, decompiled or searched for strings. The FUSE headers in `include/` were written from the public API description; their struct layouts only need to match this library.

## Protocol choice: NFSv4.0

- **Not v3.** v3 needs separate MOUNT and NLM (locking) protocols. v4.0 is one protocol on one connection, with locks.
- **Not v4.1.** The macOS client is mounted with `vers=4.0`. Sessions and the backchannel add work with no benefit for a single local client.
- **No delegations, no callback channel.** Mount option `nocallback`; `OPEN` always returns `OPEN_DELEGATE_NONE`.
- **Only what the client sends.** Implemented: `PUTROOTFH PUTFH GETFH SAVEFH RESTOREFH LOOKUP LOOKUPP GETATTR SETATTR ACCESS READLINK READDIR READ WRITE COMMIT OPEN OPEN_CONFIRM OPEN_DOWNGRADE CLOSE CREATE REMOVE RENAME LINK LOCK LOCKT LOCKU RELEASE_LOCKOWNER SETCLIENTID SETCLIENTID_CONFIRM RENEW SECINFO VERIFY NVERIFY`, plus `OPENATTR` and the named-attribute directory in the experimental `streams` mode.

## Mounting

- **Unix socket on macOS 26 and later.** The NFS client accepts a local socket: `mount_nfs -o vers=4.0,proto=ticotsord,nocallback,... '</abs/path/s>:/Volume' mnt`. The socket lives in a `0700` directory (`$TMPDIR/nfuse.XXXXXX`, falling back to `/tmp` when the path would exceed 103 bytes), so other users cannot connect. This form is not in `man mount_nfs`; it was derived from `mount_nfs`'s own error message and tested, and must be re-tested on every macOS release.
- **127.0.0.1 TCP before macOS 26.** On macOS 12.7.6 large `WRITE` requests over the unix socket fail in the client (`nfs send error 40`, EMSGSIZE) and the application gets EIO. Bigger socket buffers did not help, and `rwsize` of 32K, 64K, 128K and 256K all failed. Over TCP the same writes work, and macOS 27 works over the unix socket. Default (`NFUSE_TRANSPORT=auto`): unix socket when `kern.osproductversion` is 26 or later, TCP (random port) before; `unix` and `tcp` force one. If a unix-socket mount fails, the library retries once over TCP.
- **Two checks on TCP connections** (on by default; `NFUSE_TCP_GUARD=0` disables them, for tests):
  1. *Who owns the peer port.* At `accept`, the library looks (via `libproc`) for a process whose descriptor table holds the client end of the connection. The kernel NFS client's socket belongs to no process. A connection owned by a user process is closed, except the `mount_nfs` this process started, which is accepted as untrusted. Scanning about 177 processes took 1.6-2.9 ms; mount time did not change measurably. Without root only the caller's own processes are visible.
  2. *Untrusted until proven.* The first kernel connection that arrives during the mount handshake is trusted. Any other connection may only send `RENEW`, `RELEASE_LOCKOWNER`, `SETCLIENTID_CONFIRM`, a `SETCLIENTID` carrying the same client identifier as at mount time, or a COMPOUND that starts with `PUTFH` of a valid handle; presenting a valid handle makes the connection trusted (this is how the client reconnects). Everything else gets `NFS4ERR_WRONGSEC` or `STALE`, and the connection is closed after 8 refusals. Handles contain a 32-bit random boot number that only the mounted client has seen.
  - Measured on macOS 12.7.6: a process of the same user was refused at `accept`; with the scan disabled, `PUTROOTFH`, `SETCLIENTID` and forged handles were all refused and no handle was obtained; a process of another user was refused when the server ran as root; client reconnects were never refused.
  - Residual risk: this is not cryptographic. The handle check rests on 32 random bits; without root the first check cannot see other users' processes; a descriptor in transit over `SCM_RIGHTS` is in no descriptor table; a connection that wins the race against `mount_nfs` during the handshake and has no visible owner would be taken for the trusted one. The request parser has not been fuzzed. None of this applies to the unix-socket transport.
- **No rpcbind, no mountd.** NFSv4 has no MOUNT protocol; the first packet is `NULL`.
- **Options passed to `mount_nfs`.**
  - Read-write: `vers=4.0,<transport>,nocallback,rwsize=262144[,nobrowse],hard,intr,timeo=10,deadtimeout=60,locks[,nosuid][,nodev][,noexec][,nordirplus|,namedattr]`.
  - Read-only (`-o ro`, or no `write` callback): `vers=4.0,<transport>,nocallback,rwsize=262144[,nobrowse],rdonly,locks,soft,intr,timeo=10,retrans=10,deadtimeout=60[,...]`.
  - `<transport>` is `proto=ticotsord` or `tcp,port=<n>`. `nobrowse`, `nosuid`, `nodev` and `noexec` are passed only when the file system's `-o` list contains them. `nordirplus` is added in `adfile` mode and `namedattr` in `streams` mode (see Extended attributes). `rwsize` accepts 4096-1048576.
  - `hard` for writes: with `soft`, a timed-out `WRITE`/`COMMIT` fails silently and data is lost while the application thinks it succeeded.
  - `intr` is set, but with the server gone, SIGINT and SIGTERM did not interrupt blocked processes in testing (SIGKILL did). The guard process exists because of this.
  - `deadtimeout=60`: if the server dies, the client declares the mount dead and unmounts it. Measured 66-70 s after `kill -9` with nothing else intervening.
  - `rwsize=262144` must be explicit; without it the client uses 32 KB and ignores the server's `maxread`/`maxwrite`.
  - `locks` must be explicit on read-only `soft` mounts; the default there becomes `locallocks`, which NFSv4 rejects with EINVAL.
  - Accepted and ignored: `local allow_other allow_root fsname nonempty async kernel_cache noatime atime relatime default_permissions blkdev blksize use_ino attr_timeout entry_timeout noappledouble noapplexattr acl big_writes`. `local` has no effect: the volume is always an NFS volume to macOS (`MNT_LOCAL` clear).
- **Mount completes before `fuse_new` returns.** The handshake needs the callbacks, so the server thread starts in `fuse_new`, `mount_nfs` runs, and the thread is then parked at a request boundary. File systems commonly call `daemon()` next; threads do not survive `fork()`, but sockets do, so requests queue in the socket until `fuse_loop` restarts the thread in the child. CoreFoundation types used later are initialised before the fork.
- **A handshake is never left hanging and never interrupted.** On macOS 12.7.6, a handshake request that the server left unanswered ended in a kernel panic about 28 s later during development (the client closed the connection after about 13 s and reconnected). So: if a handshake request has been pending for 5 s with no backend progress, or 10 s regardless (`NFUSE_MOUNT_STUCK_S`), or `mount_nfs` has not returned after 30 s (`NFUSE_MOUNT_TIMEOUT_S`), the library takes over the connections and answers `NFS4ERR_IO` so that `mount_nfs` fails by itself. It does not kill `mount_nfs` and does not force-unmount while a handshake is in progress. The process then exits with code 24. Measured: handshakes blocked for 10-90 s all failed after 5.1-5.2 s with no mount left behind. Other macOS versions were not examined.
- **Unmount detection.** The client sends no RPC on unmount; it just closes the connection. `fuse_loop` returns when all connections are closed and the mount table (`getfsstat(MNT_NOWAIT)`, which never contacts the server) no longer lists a mount whose source equals this mount's spec, or when the mount has been absent for three one-second polls. Matching the source, not just the path, handles a stale mount stacked on the same path. `nfuse_end_reason()` reports `unmounted`, `device_lost` or `exit_requested`. Leftover socket directories from crashed processes (no listener, older than one minute) are removed on the next mount.

## Mapping NFS handles to FUSE paths

FUSE high-level callbacks take paths; NFS uses opaque file handles and has no `FORGET`. OpenMacFUSE keeps three structures between them.

**Node table**: inode number -> (parent inode, name). Paths are built by walking parents (up to 16 KB). The table has a cap (default 32,768 nodes, `NFUSE_MAX_NODES`). Never evicted: the root, open files, directories with child nodes in the table, nodes used by the current COMPOUND. Files go before directories, each in LRU order.

**Handles that survive eviction.** A handle is `kind(1) nanc(1) reserved(2) boot(4) ino(8) ancestors(8 x nanc)`, at most 14 ancestors, 128 bytes. When `PUTFH` names an evicted node, the library walks the handle's ancestor chain to the nearest node still in the table, then re-finds each level by inode number in the parent's directory snapshot and confirms with `getattr`. Renames would make a handle's ancestor chain stale, so:

- handle bytes never change for the life of an object (`FH4_PERSISTENT`; the client identifies vnodes by handle bytes): before a rename, the chains of the moved node and of every node under it are frozen;
- a bounded "moved" table (8,192 slots, 4-way set-associative, oldest evicted) records inode -> newest known location, and recovery consults it at each level. A direct-mapped first version lost 3 recoveries in testing; the set-associative one lost 0.

Measured with the cap at 64, 300 renames plus a directory move, then 3,000 new files to force eviction: 7,839 recoveries, 0 failures, 0 STALE on macOS 27; 1,557 / 0 / 0 on macOS 12.

**Inode numbers.** Handles are built from `st_ino`, which should be stable, unique and non-zero (the FUSE `use_ino` behaviour). If `getattr("/")` returns `st_ino == 0`, the library assigns numbers by path instead: a path keeps its number while the object exists, a rename carries the number (and everything below a renamed directory) to the new path, and a delete drops it. Changes made behind the library's back are not seen in that mode.

**Known limit:** FUSE 2.x has no generation number. If a file is deleted and its inode number is reused, a client still holding the old handle reaches the new file. Files open at deletion are silly-renamed by the client, not removed.

## Directory listing

Many FUSE file systems ignore the `offset` argument of `readdir` and list from the start every time. Serving NFS `READDIR` pages from such a callback makes listing cost grow with the square of the directory size.

The library calls `readdir` once per directory version and keeps a snapshot (name, inode, type). `READDIR` pages come from the snapshot. The cookie verifier mixes a random salt, the directory inode, its change attribute, mtime, size and a per-node generation counter bumped on every change made through the library. A changed directory gets a new verifier and a stale continuation gets `NFS4ERR_NOT_SAME`. If the snapshot was evicted but the directory did not change, it is rebuilt under the same verifier. Snapshots are capped at 16 MB in total (the one in use is exempt).

macOS 12 sometimes continues a listing with a zero verifier when two processes list the same directory. Rejecting that produced EIO in `ls`; the library accepts it.

Attributes fetched for `READDIR` are kept in an attribute cache (8,192 slots hashed by inode, 1 s default TTL, `NFUSE_ATTR_TTL_MS`), so the following `LOOKUP`/`GETATTR` do not call the file system again. Several `GETATTR`s in one COMPOUND call `getattr` once.

In `adfile` mode the mount uses `nordirplus` (reason below). The client then sends one `LOOKUP` COMPOUND per entry on the first listing: a cold `ls` of a 40,000-entry directory took 3.6 s against 1.4 s with `rdirplus` (macOS 12.7.6; 1.1 s of the 3.6 s is server time). Warm listings and `ls -l` are within 10% of each other in the two modes.

## State: clients, opens, locks, replays

- **Single local client.** Leases are never expired by the server, so sleep/wake does not lose state. Up to 8 client records.
- **Open state.** Share access/deny checks, open upgrade, `OPEN_CONFIRM`, `OPEN_DOWNGRADE`, `CLOSE`. Create modes `UNCHECKED4`, `GUARDED4` and `EXCLUSIVE4` (the verifier is stored in atime/mtime seconds as RFC 7530 allows and replaced by the client's following `SETATTR`).
- **Locks.** `LOCK`/`LOCKT`/`LOCKU`/`RELEASE_LOCKOWNER`, conflicts by lock-owner. Lock state belongs to the lock-owner, not the open: macOS 12 does not send `RELEASE_LOCKOWNER` and reuses a lock stateid after `CLOSE`. On `CLOSE`, remaining ranges are released and the stateid stays valid; idle owners are reclaimed after two lease periods. Blocking locks are answered `DENIED` and the client polls (0.2-0.4 s to acquire after release); there is no fair wait queue. `flock`, `fcntl` locks and concurrent multi-process sqlite3 writers were tested.
- **seqid replay (RFC 7530 section 9.1.7).** Each open-owner and lock-owner remembers its last seqid, operation, status and result body (up to 512 bytes). A retransmission is answered from that record.
- **Duplicate request cache.** After a reconnect the client resends unanswered requests with the same xid. COMPOUNDs containing non-idempotent operations (`OPEN OPEN_CONFIRM OPEN_DOWNGRADE CLOSE CREATE REMOVE RENAME LINK SETATTR LOCK LOCKU RELEASE_LOCKOWNER`) are cached by (xid, digest of the first 512 bytes + length): 1,024 entries / 1 MB, replies over 4 KB not cached, LRU. `WRITE` and `COMMIT` are idempotent and large, and are not cached. Measured: each of six operations had its first reply dropped and the connection closed; every command succeeded and each operation ran once.
- **Server restart.** Handle boot number and state boot number are separate. A new process makes old handles `STALE`, old stateids `STALE_STATEID`. There is no grace period; reclaims get `NO_GRACE`. macOS 27 sends `CLAIM_PREVIOUS` with the old clientid straight after `STALE_STATEID`; macOS 12 does not re-run `SETCLIENTID` after `NO_GRACE`, so the server accepts its last 4 clientids from before an in-process state reset. Both recover in under a second in the test hook.

## Reads and writes

- `WRITE` passes data straight from the receive buffer to the file system's `write`. The server never accumulates file data. `UNSTABLE4` returns immediately; `FILE_SYNC4`/`DATA_SYNC4` call the `fsync` callback first. `COMMIT` calls `fsync`. The write verifier is random per process start, so after a restart the client resends uncommitted data. `READ` reads straight into the reply buffer.
- **Durability is whatever the file system's `fsync` callback provides.** The library only passes `COMMIT` and stable writes through. If `fsync` is asynchronous, so is `COMMIT`; with no `fsync` callback both succeed without doing anything.
- **Known limit:** on macOS 12.7.6, when `COMMIT` returned `NFS4ERR_IO` (device powered off mid-flush), the application's `fsync()` and `close()` still returned 0. Not replying, and replying with a changed write verifier, did not change this; the server answers `NFS4ERR_IO` as the protocol requires. Not re-checked on other versions.
- `supported_attrs` must list the write-only attributes `time_access_set` and `time_modify_set`. Without them the client silently skips time-setting `SETATTR`s and `touch -t` succeeds without changing anything.
- Not called: `poll`, `read_buf`, `write_buf`, `ioctl`, `lock`, `flock`, `bmap`, `fallocate` (no NFSv4.0 counterpart; locks are decided by the server). `utimens` never receives `UTIME_OMIT`.

## Extended attributes

Selected with `-o nfuse_xattr=adfile|appledouble|streams`.

- **`adfile` (default with `fuse_mount`/`fuse_new`).** The client keeps its default `nonamedattr` behaviour and stores extended attributes, Finder info and resource forks in AppleDouble files named `._X`. The server does not create those files. A `._X` whose `X` exists is a view of `X`'s attributes: `LOOKUP` finds it only if `X` has attributes; `READ` synthesises the AppleDouble image from `listxattr`/`getxattr` (the resource fork part is read by offset); `CREATE`/`WRITE`/truncate go to an in-memory session that is parsed once the last open of it has closed (the client sends `COMMIT` in the middle of writing one, so not before) and applied as a difference through `setxattr`/`removexattr`; `REMOVE ._X` removes all attributes; `RENAME ._A -> ._B` moves them; `READDIR` never lists a virtual `._X`. What "an extended attribute" is on disk is up to the file system: for example, the NTFS driver in NTFSTool Pro maps these xattr callbacks to alternate data streams.
  - Requires `nordirplus`: with `rdirplus`, after a listing the client decided from its cached directory contents that `._X` did not exist and sent no `LOOKUP` (macOS 12.7.6).
  - The file system's `setxattr` must accept names such as `com.apple.FinderInfo` and `com.apple.ResourceFork`.
  - Real `._X` files already on the volume are hidden while `X` exists and merged into `X`'s attribute view. Before the first attribute write to `X` the real file is migrated into the file system's attributes and then deleted (`nfuse_adclean=1`, default) or kept with a hidden marker attribute (`nfuse_adclean=0`). Orphan `._` files (no `X`) are ordinary files.
  - On macOS 27 the client, after renaming `A` to `B`, creates `._A` under the old name, writes an AppleDouble carrying `com.apple.provenance`, and renames it to `._B`. For 5 s after a rename the old name is therefore an alias of the new file (64 entries), so nothing is created on disk. If the client is later than 5 s, a real `._` file results.
  - Bounds: 64 MB per image (larger gets EFBIG), 256 MB for all sessions, 4,096 cached clean headers. Renaming `._A` to a name not starting with `._` returns `NFS4ERR_XDEV`. Content written to `._X` that is not AppleDouble is logged and not applied.
  - Measured (suite run on macOS 12.7.6 and 27.0.1): xattr set/get/remove, Finder tags, custom icons, a 5 MB resource fork, and `cp -p`, `ditto`, `rsync -aE`, `tar --xattrs` round trips are byte-identical; after creating 1,000 files, copying and a `git clone`, the volume holds 0 `._` files and `git status`/`git fsck` are clean.
- **`appledouble`.** The client's `._X` files are stored as real files through ordinary `OPEN`/`WRITE`. On macOS 26 and later the system adds `com.apple.provenance` to new files, so nearly every new file gets a `._` sibling; on macOS 27 a git work tree on such a volume showed 48 untracked `._` files and `git fsck` reported 67 errors (fuse-t 1.0.49, another NFS-based FUSE implementation, gave the same result). In this mode the snapshot lists each `._X` immediately before `X`, because the client removes `._X` together with `X` and `rm -r` otherwise reports ENOENT for it. `fuse_main` defaults to this mode because not every file system's `setxattr` accepts un-prefixed `com.apple.*` names.
- **`streams` (experimental, off by default).** The server advertises named attributes, the mount gets `namedattr`, and attributes travel through `OPENATTR` to the xattr callbacks. Enabling named attributes on the macOS NFS client led to a kernel panic during development (seen on 27.0.1 and on 12.7.6, when an attribute was read after a long value had been replaced by a shorter one). The server now truncates at `OPEN` instead of at the first `WRITE`, after which the suite ran clean on 12.7.6; this has not been re-tested on macOS 26/27. That is why the option is off by default; use it only on a machine you can afford to crash.

## Names: case and Unicode form

- **Case.** `-o nfuse_case=insensitive|sensitive`. Default with `fuse_mount`/`fuse_new`: insensitive and case-preserving; `fuse_main` defaults to sensitive. A lookup tries the exact name first, then a per-directory name index keyed by `fold(NFC(name))`. The index is built from one `readdir`, maintained incrementally, and capped at 16 MB (LRU). Creating a name that differs from an existing one only by case acts on the existing object (`mkdir`/links get EEXIST); a case-only rename is a plain rename. Folding is per-code-point simple upper-casing from the system UTF-8 locale unless the file system registers `casefold`.
- **Automatic fallback to sensitive.** Before mounting, the tree is scanned breadth-first for at most `nfuse_case_scan_ms` (default 300, 0 disables). If two names in one directory differ only by case, the whole volume is mounted case-sensitive. If the scan runs out of time the volume stays insensitive; in a directory with such a pair found later, the exact spelling wins and both objects remain reachable. A 42,000-entry volume was scanned in 5.6 ms.
- **What the client sees.** The `case_insensitive` attribute follows the effective mode; `case_preserving` is always true. `pathconf(_PC_CASE_SENSITIVE)` returned 0 on macOS 12 and 27 in insensitive mode and 1 after a fallback, and git set `core.ignorecase` accordingly.
- **NFC/NFD.** Lookups always treat the composed and decomposed spellings of a name as equal. New and renamed names are stored in NFC (`nfuse_nfc=1`, default; 0 stores them as given). `READDIR` returns names as stored. If both spellings exist on disk, each opens its own file. Consequence: a tool comparing names byte-wise against an NFD source may see them as different.
- `nfuse_get_mount_info()` returns the effective case mode, whether the fallback happened, the xattr mode, transport, handshake time and watchdog/guard settings.

## Other Mac behaviour

- **Creation time.** `time_create`/`time_backup` map to `getxtimes`, `setcrtime`, `setbkuptime`, and are requested only when the client asks for them.
- **Flags.** NFSv4 `hidden`/`archive` map to `st_flags` (`UF_HIDDEN`, `SF_ARCHIVED`) and the `chflags` callback; without the callback they are skipped so the rest of the `SETATTR` succeeds. `chflags uchg` (Finder "Locked") fails in the client with ENOTSUP and no request is sent.
- **Not available on an NFS volume:** Trash (the system reports that the volume has none), `exchangedata`, `renamex_np(RENAME_SWAP)`, `clonefile` (all ENOTSUP; atomic-save code paths that fall back to rename were tested). Spotlight reports indexing disabled; enabling it was not tried.

## When things go wrong

All of the following take the mount down instead of leaving applications blocked on a dead volume. Timing uses `CLOCK_UPTIME_RAW`, which does not advance during system sleep.

- **Device lost: `nfuse_device_lost(f, reason)`.** The file system calls it (any thread, also from a signal handler) when its storage is gone. From then on every operation that would enter the file system, and `CLOSE`, returns `NFS4ERR_IO` (the client hands EIO to the application and does not retry); operations that touch only server state still work. `fuse_loop` returns and `fuse_unmount` goes straight to `unmount(MNT_FORCE)` (up to 3 attempts). Measured with the test hook (30 rounds, writing/reading/idle): mount gone 0.13-0.27 s after the call. With a real USB SSD powered off under a file system that detects the loss itself: 0.2-0.5 s while writing, 1.2-1.4 s idle or reading, no process left uninterruptible.
- **Watchdog thread.** Every 0.2 s it looks at the oldest request in flight (including ones waiting for the lock), the heartbeats of the connection, accept and `fuse_loop` threads, and the final `destroy`. A request older than `NFUSE_WATCHDOG_S` (default 30, 0 = off) during which the backend progress counter did not move is `stuck`; one older than `NFUSE_WATCHDOG_MAX_S` (default 600) is `too_long`; a silent thread is `conn_stall`/`accept_stall`/`loop_stall`. The progress counter comes from the `progress` callback; without it only time counts. On firing: later requests are answered `NFS4ERR_IO` without taking the lock, stuck connections are handed to takeover threads that fail queued requests, the `watchdog` callback runs, the mount is force-unmounted and the process calls `_exit` (code 24 unless the callback returns another). Nothing is cleaned up, because the stuck thread holds the lock. Measured with the threshold lowered to 5 s: mount gone 5.3-6.9 s after the hang; a request blocked for 15 s with progress did not trigger it. The slowest device tested (a USB flash drive) had at most 0.83 s without progress during a request.
- **Guard process.** `fuse_loop` forks a small child before starting the server thread (`NFUSE_GUARD=0` disables it). The child uses system calls only, closes every inherited descriptor and reads a heartbeat pipe written once per second. EOF (exit, crash, `kill -9`) with the mount still present: forced unmount. No heartbeat for `NFUSE_GUARD_S` (default 20; 0 = EOF only): forced unmount, then SIGKILL if the process has not exited after 3 s. Measured: mount gone about 2.1 s after `kill -9` and 0.02 s after an abort, against 66 s with the guard disabled. The guard holds a shared lock per mount point; a new mount on the same path waits for it (up to 10 s), because a forced unmount still in progress otherwise detached the new mount. The guard is not running during the handshake; a process killed then leaves `mount_nfs` to fail by itself (about 20 s).
- **Sleep/wake.** Seven sleep cycles of about 33 s (idle, writing, reading) on macOS 12.7.6: the volume was still mounted, first access took 0.03-0.21 s, workloads continued without error. Long sleep and lid-close were not tested.

## fuse_main, fuse_opt_parse, extension callbacks

- **Supported entry points.** `fuse_mount`, `fuse_new`, `fuse_loop`, `fuse_loop_mt` (same as `fuse_loop`; callbacks are serialised), `fuse_exit`, `fuse_unmount`, `fuse_destroy`, `fuse_get_context`, `fuse_get_session`, `fuse_set_signal_handlers`, `fuse_remove_signal_handlers`, `fuse_parse_cmdline`, `fuse_main`/`fuse_main_real`, `fuse_opt_parse`, `fuse_opt_match`, `fuse_opt_add_arg`, `fuse_opt_insert_arg`, `fuse_opt_add_opt`, `fuse_opt_add_opt_escaped`, `fuse_opt_free_args`. `fuse_opt_parse` implements the template forms of the public FUSE 2.x description (`-x`, `foo`, `foo=`, `foo=%s`, `-x %s`, `FUSE_OPT_KEY`, `KEEP`, `DISCARD`).
- **`fuse_main`.** Parses the command line, forks unless `-f`/`-d`, and mounts in the child (`init` runs in the background process, as in FUSE 2.x). The parent exits 0 once the child reports the mount, or with the child's status. Return values: 1 bad arguments, 2 no mount point, 3 `fuse_mount` failed, 4 mount failed, 5 could not go to background, 6 signal handlers, 7 loop error. It prepends the defaults `nfuse_case=sensitive,nfuse_xattr=appledouble` and `volname=<last path component>`; later options override them. `NFUSE_OPTS=<a,b=c>` appends options for programs that do not forward `-o`. If `init` ran and the mount then fails, `destroy` is called.
- **`NFUSE_WATCH_DEV=<device>`** (with `fuse_main`): a thread checks every 0.5 s that the device node and its IOKit media object still exist, calls `nfuse_device_lost` if not, and feeds the device's IOKit I/O counters to the watchdog as progress. Verified with disk images; not yet with a physical USB device.
- **`nfuse_set_ext_ops()`** (before `fuse_new`; all optional): `casefold` (the file system's own case folding); `xattr_read`/`xattr_write`/`xattr_truncate` (attribute I/O by offset; without them the library reads, modifies and rewrites whole values, up to 64 MB); `progress` (monotonic count of completed device I/Os; must be lock-free); `watchdog(why, op, secs)` (called once when the watchdog fires; must not touch the file system; returns the exit code); `report_fd` (descriptor of an append-only JSON-lines file to which the guard writes one `nfuse_guard` line).
- **`NFUSE_REPORT=<file>`** (with `fuse_main`): JSON lines `start`, `nfuse_mount`, `mount_fail`, `device_lost`, `watchdog`, `nfuse_guard`, `exit`; the `exit` line carries code 23 after a device loss. `NFUSE_LOG=<file>` writes a text log; `-o debug` logs to stderr; `NFUSE_VERBOSE=1` logs every COMPOUND.

## Test hooks

Environment variables, read only when set: `NFUSE_TEST_DROP=<op,...>` with `NFUSE_TEST_DROP_MODE=close|silent` (drop a reply, to exercise the duplicate request cache), `NFUSE_NO_DRC=1`, `NFUSE_TEST_HOOKS=1` (SIGUSR2 simulates a server restart), `NFUSE_TEST_HANG=<op>[:n]` with `NFUSE_TEST_HANG_MS` and `NFUSE_TEST_HANG_PROGRESS` (block an operation while holding the lock), `NFUSE_TEST_STALL=<file>` (stall the server thread while the file exists), `NFUSE_TEST_DEVLOST_FILE=<file>` (device lost when the file appears), `NFUSE_TEST_GUARD_NOSCAN=1` (skip the process scan), `NFUSE_TEST_RAWNAMES=1` (no name equivalence, to build test volumes), `NFUSE_NAMEDATTR=1|2` (force named attributes; 2 adds a synthetic read-only attribute), `NFUSE_NO_RDONLY=1`, `NFUSE_MOUNT_EXTRA=<opts>` (appended to the `mount_nfs` options), `NFUSE_LOG_SYNC=1`.

## Memory bounds

| Structure | Bound |
|---|---|
| Receive/send buffers | fixed pool: 2 per connection x (rwsize + 64 KB), max 4 connections; a record larger than that closes the connection |
| Node table | cap x ~100 bytes + names (default cap 32,768) |
| Attribute cache | 8,192 slots |
| Directory snapshots | 16 MB total |
| Name index | 16 MB total |
| Duplicate request cache | 1,024 entries / 1 MB |
| Moved table | 8,192 slots |
| Virtual `._` files | 4,096 clean headers; write sessions 64 MB each, 256 MB total |
| Owners, lock state | reclaimed after two idle lease periods |
| Path-assigned inode numbers (only when `st_ino` is 0) | one string per known path, not capped |

No structure grows with file size. A six-hour mixed load on macOS 12.7.6 (29 million COMPOUNDs, one file system built on the library) kept the process footprint at 50-51 MB (+0.27 MB/hour), 16 descriptors, longest COMPOUND 0.26 s; the guard process used 0.3 MB.

## Threads

One accept thread, one thread per connection, the watchdog thread, and the guard process. All COMPOUNDs run under one lock, so file system callbacks are serialised, matching single-threaded `fuse_loop`. During a slow `COMMIT`, other requests on the mount wait: with a write-plus-fsync load a 20 ms probe was delayed by at most 0.15 s on a USB SSD, and one request took 2.2 s on a slow USB flash drive. Parallel read paths and asynchronous `COMMIT` replies are not implemented.

## Tested systems

| macOS | Hardware | Coverage |
|---|---|---|
| 12.7.6 | Intel | full suite (TCP transport): read/write, replay, eviction, Mac semantics, device loss, watchdog, guard, local-connection checks, sleep/wake, six-hour load |
| 27.0.1 | Apple silicon | unix-socket transport: normal read/write, simulated unplug, kill, Mac-semantics suite |
| 13, 14, 15, 26 | - | untested |

Most measurements above were taken with disk images; power-off, watchdog and sleep tests also used a USB SSD. Spinning disks were not tested; the 30 s watchdog default relies on the progress counter to tolerate spin-up.

## Not implemented

The low-level FUSE API, FUSE 3. Delegations. Kerberos (`sec=sys` only). Multiple exports per process. Server-side ACLs. A fair queue for blocking locks. Stronger (keyed) handle authentication for the TCP transport. Fuzzing of the request parser.
