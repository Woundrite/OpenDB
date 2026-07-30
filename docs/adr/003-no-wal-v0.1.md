# ADR-003: No WAL in v0.1

## Status

Accepted (Phase 4 design decision; revisited in Phase 5 with the
CrashDurable capability flag).

## Context

A write-ahead log (WAL) is the standard durability mechanism for crash
recovery in a relational database. It records every staged change to a
sidecar file before the data file is touched, so on restart we can
replay uncommitted-but-fsync'd work.

The user explicitly accepted "WAL deferred to Phase 5" in the Phase 4
design. Phase 5 landed CrashDurable as a **capability flag** rather than
implementing a full WAL sidecar.

## Decision

v0.1 implements CrashDurable via the `commit() → saveMetadata() →
pager.sync()` barrier:

1. `prepare(txn)` flushes the BTree pages to the data file.
2. `commit(txn, visibleSeq)` stamps staged versions with `visibleSeq`.
3. `saveMetadata()` rewrites page 1 with the new root IDs + visibleSeq.
4. `pager.sync()` fsyncs.

This is sufficient for the most common crash scenario: a clean restart
with no in-flight writes observes the last fully-committed state.
Staged entries from an aborted txn are MVCC-invisible (`commitSeq==0`
and never committed), so they don't pollute restart.

It is **not** sufficient for:
- A crash *during* `saveMetadata()` between per-tree flush and metadata
  save: the data file has new rows but the metadata page might still
  reference old root page IDs. The metadata page 1 has its own CRC, so
  on restart we'd detect the corruption and refuse to open.

## Consequences

- Code is dramatically simpler than WAL replay logic.
- The metadata page 1 is the durable atomicity barrier.
- Crash-during-metadata-save requires manual recovery (delete the
  metadata page and lose the post-commit rows on that table).
- A future enhancement is to add `<file>.wal` recording staged ops for
  partial-recovery scenarios — left as Phase 6+ work.

## What the CrashDurable capability flag means

`StorageCapability::CrashDurable` is **honest** — it means the file is
in a state from which the last committed state can be reconstructed.
It does NOT mean "any partially-written data is recoverable."
