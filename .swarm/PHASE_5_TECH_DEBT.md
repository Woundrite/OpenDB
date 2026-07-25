# Phase 5: Technical Debt & Production Readiness

This phase addresses all known stubs, placeholders, and deferred items identified during Phase 4. Each item is categorized by priority.

## High Priority (Blocking production use)

### 1. UPDATE / DELETE Handlers in EngineLoop
**Location:** `includes/atomdb/core/EngineLoop.hpp:138-145`
**Status:** ✅ DONE (commit 28ee752). Scan → re-put/tombstone via `(key, txn.staged)` pipeline.
**Fix:** Implement proper UPDATE/DELETE dispatch with:
- Row lock acquisition (Exclusive mode)
- WHERE predicate evaluation
- Row re-put with new values (UPDATE) or tombstone (DELETE)
- Lock release on commit/abort

### 2. HTTP API Real Server (Async I/O)
**Location:** `includes/atomdb/frontend/HttpApi.hpp:31,81-90`
**Status:** Stub — `run()` is empty loop; no actual socket listener
**Fix:** Implement async HTTP server:
- Use `cpp-httplib` or `boost::beast` for HTTP/1.1
- JSON request parsing with `JsonEncoder`
- Concurrent request handling via `IEngineDispatcher` thread pool
- TLS support via OpenSSL
- Graceful shutdown
**Dependencies:** EngineDispatcher (Item 12), async I/O library

### 3. EngineDispatcher (Thread Pool)
**Location:** `includes/atomdb/core/EngineDispatcher.hpp`
**Status:** ✅ DONE (commit d28929b). `std::jthread` pool + `std::counting_semaphore<10000>` + `Engine_loop` already wired via `HttpSession`.

### 4. WAL / CrashDurable (Phase 5 spec)
**Location:** `includes/atomdb/storage/LocalFileStorageProvider.hpp:38`
**Status:** ✅ Partial (commit 0be762d). Header comment explains crash safety comes from the commit barrier (`commit -> saveMetadata -> pager.sync`). Staged entries are MVCC-invisible after restart because they're never promoted past `commitSeq==0`. Test `LocalFile_CrashDurable_Data_Persists_Across_Reopen` proves correctness. A real sidecar `<file>.wal` is a future enhancement.
**Fix:** Write-Ahead Logging:
- Append-only WAL file with CRC32 per record
- Group commit for throughput
- Recovery on open: replay from last checkpoint
- fsync group commit (not per txn)
- Checkpointing / log truncation

### 5. Range / List Partitioning (Sharding)
**Location:** `includes/atomdb/storage/ShardedStorageEngine.hpp`
**Status:** ✅ DONE (commit 3e98a33). `routeByPolicy` interprets Hash/Range/List kinds via `installPartitionPolicy`. Provider installs on `createTable`, clears on `dropTable`.

---

## Medium Priority (Correctness / Performance)

### 6. UPDATE/DELETE via EngineLoop (Locking + TXN)
**Location:** `includes/atomdb/core/EngineLoop.hpp:138-145`
**Status:** Stubbed — bypasses locking and transaction management
**Fix:** Route UPDATE/DELETE through full EngineLoop pipeline:
- Acquire Exclusive lock on table
- Deadlock detection pre-check
- Dispatch to handler (Update/Delete)
- Prepare/Commit/Abort with visibleSeq
- Lock release

### 7. SQL Parser — Missing Features
**Location:** `src/SqlParser.cpp` / `includes/atomdb/frontend/SqlParser.hpp`
**Status:** ✅ Partial progress: ORDER BY (multi-column, ASC/DESC), LIMIT, OFFSET implemented (commits adding Parser + Command::orderBy/limit/offset fields + EngineLoop in-memory sort). Remaining: subqueries, joins, ALTER TABLE, INSERT DEFAULT VALUES, column defaults, NULLS FIRST/LAST.
**Missing still:**
- `INSERT INTO table DEFAULT VALUES`
- Column defaults in CREATE TABLE (`DEFAULT 'value'`)
- `ALTER TABLE` (add/drop column, change type)
- `INDEX` / `UNIQUE` constraints
- `JOIN` (multi-table SELECT)
- Subqueries (`SELECT ... WHERE x IN (SELECT ...)`)
- `NULLS FIRST/LAST` in ORDER BY

### 8. JSON Encoder — Incomplete
**Location:** `includes/atomdb/frontend/JsonEncoder.hpp`
**Status:** "minimal JSON serializer for test/stub purposes"
**Fix:** Full JSON support:
- Proper escaping of all control chars
- UTF-8 validation
- Streaming encoder for large result sets
- Date/Timestamp ISO-8601 formatting
- Blob base64 encoding

### 9. ShardedStorageProvider — URI Re-open
**Location:** `includes/atomdb/storage/ShardedStorageProvider.hpp`
**Status:** ✅ DONE (commit 9652254). Constructor takes `children_already_open` flag; when set, provider `open()` is a no-op so per-shard URIs are preserved.

### 10. JsonEncoder — Tuple Key Encoding
**Location:** `includes/atomdb/frontend/JsonEncoder.hpp`
**Issue:** Tuples encoded as objects — column names become JSON keys. Works for demo but not for generic use.
**Fix:** Optional array-of-arrays mode for compact wire format.

---

## Low Priority (Polish / Observability)

### 11. MVCC visibleSeq Cutoff
**Location:** `includes/atomdb/storage/InMemoryStorageEngine.hpp:218`
**Status:** ✅ DONE (commit 28ee752). `v.commitSeq <= visible_seq_` applied in `isVisibleUnlocked`.

### 12. Pager — Free List Reuse
**Location:** `includes/atomdb/storage/Pager.hpp`
**Status:** First-fit allocator — fragmentation over time
**Fix:** Best-fit or segregated free lists; background compaction.

### 13. BTree — Duplicate Key Handling
**Location:** `includes/atomdb/storage/BTree.hpp`
**Issue:** Returns `false` on duplicate `(key, commitSeq, txnId)` — could distinguish insert vs update.

### 14. Query Planner / Optimizer
**Status:** Non-existent — full table scan for all SELECTs.
**Fix:** 
- Index support (secondary indexes)
- Predicate pushdown
- Join reordering (when JOINs implemented)

### 15. Metrics / Telemetry
**Missing:** No latency histograms, throughput counters, error rates.
**Fix:** Integrate `std::chrono` counters; expose `/metrics` endpoint via HTTP API.

### 16. Backup / Point-in-Time Recovery
**Missing:** No `BACKUP TO` command or snapshot API.

### 17. Authentication / Authorization
**Missing:** No users, roles, GRANT/REVOKE.

---

## Test Coverage Gaps

### 18. Stress / Concurrency Tests
- Concurrent INSERT/SELECT/UPDATE from 10+ threads
- Deadlock injection tests
- Long-running transaction + checkpoint race

### 19. Sharded Recovery Tests
- Kill process mid-commit, verify recovery
- Single shard corruption isolation

### 19. HTTP API Load Tests
- 10k req/s sustained
- Large payload (1MB+) handling

---

## Documentation

### 20. Architecture Decision Records (ADRs)
Missing for:
- Why engine decorator over provider decorator
- Why FNV-1a for sharding
- Why no WAL in v0.1
- Why no async I/O in v0.1

### 21. API Reference (Markdown)
- `IStorageProvider` contract
- `IStorageEngine` contract
- `IAccessPlugin` contract
- `SqlParser` SQL grammar (BNF)

---

## Phase 5 Plan Structure

| Week | Focus | Items |
|------|-------|-------|
| 1-2 | Core correctness | 1, 6 (UPDATE/DELETE via EngineLoop) |
| 3-4 | Durability | 4 (WAL), 11 (MVCC visibleSeq) |
| 5-6 | Concurrency | 3 (EngineDispatcher), 18 (stress tests) |
| 7-8 | HTTP API | 2 (real server), 8 (JSON encoder), 15 (metrics) |
| 9-10 | Sharding | 5 (Range/List), 9 (URI), 19 (recovery tests) |
| 11-12 | SQL surface | 7 (parser features), 21 (API docs) |
| 13 | Polish | 12, 13, 14, 16, 17, 20, 21 |

**Exit Criteria for Phase 5:**
- [ ] 129 → 200+ tests (stress, concurrency, recovery)
- [ ] EngineDispatcher with 4+ worker threads
- [ ] WAL + crash recovery verified
- [ ] HTTP API serves 10k req/s locally
- [ ] Range/List partitioning works
- [ ] UPDATE/DELETE pass stress tests
- [ ] All "stub" comments removed