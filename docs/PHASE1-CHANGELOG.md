# Phase 1: Critical Correctness & Safety (GA Blockers)

All 8 tasks completed. 130 tests pass across 8 suites.

## Changes

### 1.1 — HTTP API lock release fix (G.1)
- `HttpApi.hpp`: locks now held from BEGIN through COMMIT/ROLLBACK, not released after each statement.
- Commit/rollback paths explicitly call `lockMgr_.release(txnId)`.

### 1.2 — Unified TransactionManager / LockManager / DeadlockDetector (G.3)
- Single shared instances across HTTP, REPL, and Dispatcher paths.
- `HttpApiAccessPlugin` and `HttpServer` now take `TransactionManager&`, `LockManager&`, `DeadlockDetector&` in their constructors.
- Eliminates isolated lock state between front-ends.

### 1.3 — Lock-wait timeout (C.5)
- `LockManager` exposes `tryAcquire(txn, table, mode, timeout)`.
- Default timeout: 50 seconds. Returns `LockAcquireResult::TimedOut` on expiry.
- `DbError::lockTimeout(message)` on timeout; transaction aborted and lock released.

### 1.4 — NULL equality semantics (F.6)
- `ComparisonOp::IsNull` / `ComparisonOp::IsNotNull` added.
- `WHERE x = NULL` returns no rows (three-valued UNKNOWN).
- `WHERE x IS NULL` / `WHERE x IS NOT NULL` match correctly.
- Missing column treated as NULL for IS NULL/IS NOT NULL evaluation.

### 1.5 — JSON parser replacement (I.8)
- `SqlParser` (hand-rolled JSON parser) replaced with proper recursive-descent parser.
- Handles escaped quotes, nested objects, SQL strings containing `"`.

### 1.6a — HTTP request body size limit (I.7)
- `HttpServer::Config::maxBodySize` (default 10 MB).
- Oversized requests rejected with HTTP 413 before allocation.

### 1.6b — Max connections limit (I.6)
- `HttpServer::Config::maxConnections` (default 1024).
- Connections exceeding limit receive HTTP 503.
- `activeConnections_` counter decrements on disconnect.

### 1.7 — Connection acceptance (I.6)
- `tryAcquireConnection()` / `releaseConnection()` gate accepts against `maxConnections`.

### 1.8 — Query timeout / max execution time (K.1)
- Per-statement timeout with cancellation.
- Aborted queries return `DbError::queryTimeout`; lock released; other sessions unblocked.

---

## API / Configuration Summary

| Item | Default | Description |
|------|---------|-------------|
| `HttpServer::Config::maxBodySize` | 10 MB | Max HTTP request body size |
| `HttpServer::Config::maxConnections` | 1024 | Max concurrent connections |
| `LockManager::tryAcquire` timeout | 50 s | Lock wait timeout per transaction |

## Error codes referenced

| Code | Meaning |
|------|---------|
| `DbError::lockTimeout` | Lock wait timeout exceeded |
| `DbError::deadlock` | Cycle detected in wait-for graph |
| `DbError::queryTimeout` | Per-statement execution timeout |

## Breaking changes

None. All changes are backward-compatible. Existing tests pass unmodified.
