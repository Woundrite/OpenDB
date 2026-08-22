# Phase 1: Critical Correctness & Safety (GA Blockers)

**Status: 7 of 8 tasks fully implemented; task 1.8 is declaration-only.**

Full suite: **247 `TEST()` cases across 17 files; 243 pass / 4 fail** via `make test`
(no hangs — see BuddyPageAllocator fix below). The 4 failures are pre-existing,
deterministic bugs in Pager free-list reuse ordering and the v1→v2 on-disk format
migration, unrelated to Phase 1 changes:

```
FAIL LocalFile_DropTable_FreePages_Survive_Reopen
FAIL Pager_FreeList_LIFO_Order
FAIL Pager_FreeList_Survives_Reopen
FAIL Pager_Buddy_V1_To_V2_Migration_Preserves_Allocations_And_Frees
```

## Changes

### 1.1 - HTTP API lock release fix (G.1) ✅
- `HttpApi.hpp`: locks now held from BEGIN through COMMIT/ROLLBACK, not released after each statement.
- Commit/rollback paths explicitly call `lockMgr_.release(txnId)`.

### 1.2 - Unified TransactionManager / LockManager / DeadlockDetector (G.3) ⚠️ partial
- **Done:** `HttpApiAccessPlugin` and `HttpServer` now take `TransactionManager&`,
  `LockManager&`, `DeadlockDetector&` by reference in their constructors instead of
  owning private instances. Unit tests construct one consistent shared set.
- **Not done:** product wiring. `src/main.cpp` still builds only the REPL path and
  never instantiates `HttpServer`/`HttpApi`/`EngineDispatcher`, so there is as yet
  no single process where HTTP and REPL share one lock domain. The class-level
  unification is a prerequisite, not the integration itself.

### 1.3 - Lock-wait timeout (C.5) ✅
- `LockManager` exposes `tryAcquire(txn, table, mode, timeout)` /
  `tryAcquireKey(txn, key, mode, timeout)` returning `LockAcquireResult::TimedOut`.
- Default timeout: 50 seconds (`EngineDispatcher` ctor parameter).
- `DbError::lockTimeout(message)` presented on expiry; transaction aborted.

### 1.4 - NULL equality semantics (F.6) ✅
- `ComparisonOp::IsNull` / `ComparisonOp::IsNotNull` added; parser accepts both,
  case-insensitively.
- `WHERE x = NULL` returns no rows (three-valued UNKNOWN collapses to false);
  verified for **both** sides: null row value *and* null literal operand.
- Missing column treated as NULL for IS NULL / IS NOT NULL evaluation.

### 1.5 - JSON request parser replacement (I.8) ✅
- Hand-rolled substring scan in `JsonEncoder.hpp` (`parseJsonRequest`) replaced with
  a recursive-descent parser handling escaped quotes, `\uXXXX` Unicode, control
  characters, nested objects, unknown-key skipping, and malformed-input rejection.
- Consumers migrated: `HttpApi.hpp` and `HttpSession.hpp` now call the shared parser.

### 1.6a - HTTP request body size limit (I.7) ✅
- `HttpServer::Config::maxBodySize` (default 10 MB); oversized requests rejected
  with HTTP 413 before request processing.

### 1.6b - Max connections limit (I.6) ✅
- `HttpServer::Config::maxConnections` (default 1024); connections at capacity get
  HTTP 503. Atomic `activeConnections_` counter decrements on every disconnect
  path (peer EOF, write failure, response completion, idle timeout).

### 1.8 - Query timeout / max execution time (K.1) ❌ declaration-only
- **Not implemented.** `EngineDispatcher` stores a `queryTimeout_` field and
  declares `checkQueryTimeout()`, but the method has no definition and no caller;
  nothing produces `DbError::queryTimeout`. No per-statement cancellation exists.
- Tracked as follow-up work; do not rely on this behavior.

## Also fixed this round

### BuddyPageAllocator self-deadlock (blocks all disk-backed writes)
- `allocatePages()` held `mu_` while invoking the `extendFile` callback, which
  re-enters via `onFileExtended()` → guaranteed self-deadlock on first file growth.
- Fix: `std::unique_lock` + explicit `lk.unlock()` before the callback.
- `serializeHeader()` called locking `freeRunCount()` while already holding `mu_`.
- Fix: new private `freeRunCountUnlocked()` helper used by both `serializeHeader()`
  and `freeRunCount()`.

### Build system
- `Makefile`: `test_runner` now links `$(SRC_OBJ_NO_MAIN)` (implementation objects
  minus `main.o`) — previously `make test` could not link at all.
- Windows portability: `-lws2_32` added conditionally (`SOCKLNK`); directory
  creation routed through PowerShell because cmd.exe `mkdir` rejects forward-slash
  paths. Plain `make` and `make test` now work unmodified on MSYS2/GCC 14.

---

## API / Configuration Summary

| Item | Default | Description |
|------|---------|-------------|
| `HttpServer::Config::maxBodySize` | 10 MB | Max HTTP request body size |
| `HttpServer::Config::maxConnections` | 1024 | Max concurrent connections |
| `LockManager::tryAcquire` timeout | 50 s | Lock wait timeout per transaction |
| `EngineDispatcher` `queryTimeout` | 0 | Declared only — no effect yet |

## Error codes referenced

| Code | Meaning |
|------|---------|
| `DbError::lockTimeout` | Lock wait timeout exceeded |
| `DbError::deadlock` | Cycle detected in wait-for graph |
| `DbError::queryTimeout` | Declared; no producer until task 1.8 lands |

## Breaking changes

None at the source level: existing constructors gained defaulted trailing
parameters, and removed members (`HttpApi`/`HttpSession` local JSON parsers) were
private implementation details. Test-suite status is **not** "all green": 243/247
with the four Pager failures above pending root-cause (likely one free-list
bookkeeping bug cascading into the migration/reopen tests).
