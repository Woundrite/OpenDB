# AtomDB
Swarm: mega
Phase: 6 [PENDING] | Updated: 2026-07-31

---

## Phase 1: Phase 1-3 (DONE) [COMPLETED]
- [x] 1.0: Already completed: core types, engine, in-memory storage, REPL, smoke test. [SMALL]

---

## Phase 2: Phase 4 Items 1-4, 7-8 (DONE) [COMPLETED]
- [x] 2.0: Already completed: extended Value (Blob/Date/Timestamp), Schema/PartitionPolicy, IStorageProvider contract, IAccessPlugin contract, InMemoryStorageProvider composition refactor, Pager.hpp + Pager.cpp with 4 KiB CRC32 pages + free-list. [SMALL]

---

## Phase 3: Phase 4 Item 9: BTree + LocalFileStorageProvider [COMPLETED]
- [x] 3.1: Fix Pager::openFile / Pager::Pager constructor to clear the fstream error state after a failed loadHeader read on a freshly created 0-byte file. [SMALL]
- [x] 3.2: Audit and fix BTree.cpp + BTree.hpp so all 14 BTree tests pass. [SMALL]
- [x] 3.3: Audit and fix LocalFileStorageProvider.hpp so all 18 LocalFile tests pass. [SMALL]
- [x] 3.4: Finalize build.ps1 fix so the test runner links src/*.obj (except src/main.cpp) alongside test objects. [SMALL]
- [x] 3.5: Run full test suite via build.ps1 test; all tests pass. [SMALL]
- [x] 3.6: Commit Phase 4 Item 9 work. [SMALL]

---

## Phase 4: Phase 4 Items 5-6, 10+ (REMAINING) [COMPLETED]
- [x] 4.1: Item 5: CachingStorageProvider - read-cache decorator wrapping another IStorageProvider. Declares capability Caching, forwards DDL/engine, intercepts get/scan with in-memory cache keyed by (table, key, visibleSeq). Invalidate on put/remove/commit. [SMALL]
- [x] 4.2: Item 6: ShardedStorageProvider - hash/range/list partitioning across multiple IStorageProvider children. PostgreSQL-style PARTITION BY HASH/RANGE/LIST using Schema::PartitionPolicy. Routes put/get/remove/scan. [SMALL]
- [x] 4.3: Item 10: SQL parser access plugin - recursive-descent parser turning SQL subset (CREATE TABLE, INSERT, SELECT, UPDATE, DELETE, BEGIN, COMMIT, ROLLBACK, DROP TABLE) into Command objects. [SMALL]
- [x] 4.4: Item 11+: HTTP API access plugin - HTTP/JSON plugin exposing /query, /begin, /commit, /rollback endpoints mapping to Command objects. [SMALL]
- [x] 4.5: EngineDispatcher: thread-pool dispatcher accepting ISession from IAccessPlugin; worker threads run concurrently. [SMALL]
- [x] 4.6: Phase 4 final commit and retrospective. [SMALL]

---

## Phase 5: Technical Debt & Production Readiness [COMPLETED]
- [x] 5.0: Phase 5 tech-debt items 1-21 closed. UPDATE/DELETE via EngineLoop, real HTTP server (multi-threaded async I/O), EngineDispatcher pool, CrashDurable behavior, Range/List partitioning, SQL surface (ORDER BY, LIMIT, OFFSET, DEFAULTs), JSON encoder (Blob base64, ISO-8601), Metrics, backupTo, ADRs, API.md. Test count grew from 149 -> 203, all green. [SMALL]

---

## Phase 6: SQL Surface Expansion + Storage Polish [ACTIVE]
**Audit note (2026-07-31):** 4 of 6 Phase 6 items are already implemented in the working tree:
- 6.6 HttpServer select-on-listen-fd -> DONE (acceptor IS io thread 0; listen_fd_ wired via setListenFd)
- 6.1 ALTER TABLE / AlterSpec -> DONE (Schema.hpp, IStorageProvider::alterTable in InMemory + LocalFile + Sharded, parser handles ADD/DROP/RENAME, tests in LocalFileStorageProvider.test.cpp)
- 6.3 NULLS FIRST/LAST -> DONE (OrderBySpec::nullsFirst in Command.hpp, EngineLoop.hpp implements PostgreSQL defaults, parser handles syntax, SqlParser.test.cpp + EngineLoop.test.cpp cover)
- 6.4 BTree per-page recycling -> DONE (BTree.cpp:573 calls pager_.freePage(id); freeListSize + freePageCount tests in Pager.test.cpp + LocalFileStorageProvider.test.cpp)

**Remaining (ordered to avoid conflicts):**
- [x] 6.2: SQL JOINs - SELECT ... FROM a [INNER|LEFT] JOIN b ON a.x = b.y [WHERE ...] [ORDER BY ...] [LIMIT ...] [OFFSET ...]. Surface change: SqlParser grammar extension + Command::fromTables + nested-loop executor in EngineLoop. No optimizer; full table scan on each side. Tests: INNER basic, LEFT preserves unjoined LEFT rows, ON with predicate, 3-table join, JOIN on InMemory + LocalFile, sharded join. [MEDIUM] -- DONE (2026-07-31). 6 new tests pass; full suite 209/209 green.
- [x] 6.5: Pluggable buddy page allocator. Introduced `IPageAllocator` interface (`contracts/IPageAllocator.hpp`) with `BuddyPageAllocator` implementation (`storage/BuddyPageAllocator.hpp/.cpp`). Binary buddy allocator with slab classes 1,2,4,8,16,32,64,128,256 pages. O(1) ops under single mutex, ~50% internal fragmentation on odd requests, coalesces on free. `Pager` composes `unique_ptr<IPageAllocator>` (defaults to Buddy). On-disk format v2: page 0 offset 12 holds 16-byte diagnostic header; v1 singly-linked free-list chain preserved for backward compat. Automatic v1→v2 migration on open. 7 new tests added (216 total). [MEDIUM] -- DONE (2026-08-05). Full suite 216/216 green under -Werror.
- [x] 6.7: Phase 6 final commit and retrospective. Full suite 216/216 green under -Werror. All Phase 6 deliverables shipped: ALTER TABLE (6.1), SQL JOINs (6.2), NULLS FIRST/LAST (6.3), BTree per-page recycling (6.4), pluggable buddy allocator (6.5), HttpServer accept-on-io-thread (6.6). README.md, API.md, SWARM_PLAN.md updated. [SMALL] -- DONE (2026-08-05).
