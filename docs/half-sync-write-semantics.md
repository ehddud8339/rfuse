# Half-sync Write Semantics (Current Patch)

## Scope
This document summarizes semantics of the current `writethrough + async write (half-sync)` patchset with `writectr/FUSE_NOWRITE` integration.

## Semantics Preserved
- `write-before-fsync` ordering (same inode): `fsync` drains in-flight async writes before sending FUSE fsync.
- `write-before-close/flush` ordering (same inode): `flush/close` can wait for previously submitted async writes to finish.
- Read-after-write on page-cache hit: when the same cached page is read, written bytes are visible.
- Local EOF visibility for extending writes: async path updates local `i_size` earlier than completion-only update.

## Semantics Not Fully Preserved
- `write()` completion meaning is weakened:
  - sync model: closer to "server processed write"
  - current async model: "request accepted/submitted to background queue"
- Delayed error propagation:
  - daemon-side late error/short write is not always reflected in the original `write()` return value.
- Full-write visibility atomicity is not guaranteed:
  - large write may be split into multiple async requests and observed in partial state.
- FD-level strictness is weaker than inode-level strictness:
  - accounting/barrier is inode-scoped (`writectr`), not a per-fd commit tracker.
- Cache-miss/revalidation paths can still observe timing gaps vs page-cache-hit reads.

## Practical Interpretation
- Current patch restores important ordering at `fsync/close` boundaries.
- It does **not** restore full sync-write semantics for `write()` syscall completion and error model.
