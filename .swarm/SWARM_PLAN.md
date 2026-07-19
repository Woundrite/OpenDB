# AtomDB
Swarm: mega
Phase: 1 [PENDING] | Updated: 2026-07-12T11:36:40.456Z

---
## Phase 1: Phase 1-3 (DONE) [PENDING]
- [ ] 1.0: Already completed: core types, engine, in-memory storage, REPL, smoke test. 46 tests pass. [SMALL]

---
## Phase 2: Phase 4 Items 1-4, 7-8 (DONE) [PENDING]
- [ ] 2.0: Already completed: extended Value (Blob/Date/Timestamp), Schema/PartitionPolicy, IStorageProvider contract, IAccessPlugin contract, InMemoryStorageProvider composition refactor, Pager.hpp + Pager.cpp with 4KiB CRC32 pages + free-list. 53 tests pass. [SMALL]

---
## Phase 3: Phase 4 Item 9: BTree + LocalFileStorageProvider [PENDING]
- [ ] 3.1: Fix Pager::openFile / Pager::Pager constructor to clear the fstream error state after a failed loadHeader read on a freshly created 0-byte file, so the subsequent writeRaw/flushHeader succeeds. Without this fix every brand-new database file fails to initialize on Windows + libstdc++ because failbit/eofbit remain set on the fstream after the short read. [SMALL]
- [ ] 3.2: Audit and fix BTree.cpp + BTree.hpp so all 14 BTree tests pass: createNew, put+get single row, get missing, staged visibility, committed visibility by visibleSeq, tombstone, multiple versions DESC ordering, scan, empty scan, put 200 rows triggers splits, duplicate put returns false, persistence across Pager instances, text keys lexicographic sort, scan stops when callback returns false. [SMALL]
- [ ] 3.3: Audit and fix LocalFileStorageProvider.hpp so all 18 LocalFile tests pass: name/capabilities, type vocabulary, engine() non-null, createTable registers schema, duplicate rejection, dropTable, tables() listing, put+get round-trip, auto-key Int64, put 50 rows scan in order, remove creates invisible tombstone after commit, data persists across close+reopen, multiple tables+schemas persist, overwrite persists last visible value, abort leaves table intact, close without open safe, put without open fails, Blob persists across reopen. [SMALL]
- [ ] 3.4: Finalize build.ps1 fix so the test runner links src/*.obj (except src/main.cpp) alongside test objects. Must remove any pre-existing src/main.obj before linking. Already in working tree; verify and finalize. [SMALL]
- [ ] 3.5: Run full test suite via build.ps1 test and confirm all 53 existing tests + 14 BTree tests + 18 LocalFile tests = 85 tests pass. Address any remaining failures, especially persistence/reload and commit-then-scan-staged-entry logic. [SMALL]
- [ ] 3.6: Commit all Phase 4 Item 9 work (BTree.hpp, BTree.cpp, LocalFileStorageProvider.hpp, BTree.test.cpp, LocalFileStorageProvider.test.cpp, Pager.hpp fixes, build.ps1 fix) with a descriptive commit message. [SMALL]

---
## Phase 4: Phase 4 Items 5-6, 10+ (REMAINING) [PENDING]
- [ ] 4.1: Item 5: CachingStorageProvider - read-cache decorator wrapping another IStorageProvider. Declares capability Caching, forwards DDL/engine, intercepts get/scan with in-memory cache keyed by (table, key, visibleSeq). Invalidate on put/remove/commit. New tests CachingStorageProvider.test.cpp. [SMALL]
- [ ] 4.2: Item 6: ShardedStorageProvider - hash/range/list partitioning across multiple IStorageProvider children. PostgreSQL-style PARTITION BY HASH/RANGE/LIST using Schema::PartitionPolicy. Routes put/get/remove/scan. New tests ShardedStorageProvider.test.cpp. [SMALL]
- [ ] 4.3: Item 10: SQL parser access plugin - recursive-descent parser turning SQL subset (CREATE TABLE, INSERT, SELECT, UPDATE, DELETE, BEGIN, COMMIT, ROLLBACK, DROP TABLE) into Command objects. ICommandSource or IAccessPlugin/ISession. New tests SqlParser.test.cpp. [SMALL]
- [ ] 4.4: Item 11+: HTTP API access plugin - minimal HTTP/JSON plugin exposing /query, /begin, /commit, /rollback endpoints mapping to Command objects. Stub if C++23 networking stdlib not available. New tests HttpApi.test.cpp. [SMALL]
- [ ] 4.5: EngineDispatcher: replace single-threaded EngineLoop with a thread-pool dispatcher accepting ISession from IAccessPlugin; worker threads run concurrently. Honor IStorageProvider::Concurrent capability. New tests EngineDispatcher.test.cpp. [SMALL]
- [ ] 4.6: Phase 4 final commit and retrospective. Run full suite, update README.md mentioning Phase 4 deliverables, commit. [SMALL]
