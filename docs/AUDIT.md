# OpenDB — Merged Defect & Debt Catalog (AI audit + team audit)

**Ground truth:** HEAD `98106b1`. Build clean + **249/249 tests pass** on Windows MSYS2 (GCC 14).
On Linux GCC, a clean `make` **fails** (K1). Live HTTP server **crashes on the first POST with a body** (L1–L4).
Green suite coexists with a DOA server because no test drives a body POST through the real socket path (N1).

**Fix critical path (agreed):**

```
K1 → L1–L4 + N1(test) → B3 (live-verify) → B1 → B2 → B4/B8 → C2/C3 → docs sweep (A-series) → Phase 2 (WAL)
```

B-series doc examples are blocked by TWO stacked defects: L1/L2 (body stripped → parse error) AND B1
(positional columns → empty named reads). Doc claims (A5) only become true after both land.
Link 1 expanded to **K1+K2** per the independent audit — K2 was masked by K1 and blocks the same
"plain `make` succeeds" acceptance criterion.

---

## K. Build & toolchain

| # | Item | Evidence | Severity |
|---|------|----------|----------|
| K1 | `make` with default `-Werror` fails on Linux GCC: `-Wformat-truncation` false positive on `snprintf` in `dateToIso`/`timestampToIso` (`includes/opendb/types/Value.hpp:~369,~384`, surfaces via any TU including `Value.hpp`). Undocumented workaround: `make WARNINGS="-Wall -Wextra -Wpedantic"`. Windows MSYS2 unaffected. **Do NOT fix by dropping `-Werror` globally.** Verify: plain `make` produces both binaries, 249/249 unchanged. | team live repro | HIGH — **FIXED chunk 1 (`eca7e21`), confirmed by independent Linux re-run** |
| K2 | `src/SocketUtils.cpp`: `g_wsaInitialized`/`g_wsaMu` declared unconditionally, used only inside `#if defined(_WIN32)` → `-Werror=unused-variable` on non-Windows. Pre-existing since Phase 5 (`72e0a2a`); masked by K1 because the build aborted at `Value.hpp` first. Fix: declarations moved inside the `_WIN32` guard; POSIX keeps an inline no-op `ensureWsa()`. | independent audit (Linux GCC) | HIGH — **FIXED chunk 1 (second commit)** |
| K3 (candidate) | One observed `-j8` from-clean failure (clean → parallel make exited 2 after directory-creation output only; immediate serial rerun EXIT=0). Suspected order-only `mkdir` race between the two object dirs under `-j` from clean. Serial documented path (`make`, `make test`) verified deterministic. Observed once, MSYS2 — needs isolation; **must not** be masked by "rerun until green". | local observation | MEDIUM (build reliability) |

## L. Transport layer (all CRITICAL — server dies on first body POST)

| # | Item | Evidence |
|---|------|----------|
| L1 | `Content-Length:` off-by-one: literal is 15 chars, parser skips `clPos + 14` → substring starts at `:` → `stoull` throws → swallowed → `contentLength = 0` for every request | `src/HttpServerIoThread.cpp:~291-292`. Fix: `std::strlen("Content-Length:")`, no magic number |
| L2 | Consequence: `totalExpected` excludes the body (`~:298`) → JSON payload stripped → downstream parse fails on every body POST | derived from L1 |
| L3 | Error write-back segfaults: read loop moves `unique_ptr<Connection>` out of `conns_` during `handleRead()` but leaves the key → `IoThread::writeTo()` (called synchronously from that same stack) finds present-but-null entry and dereferences it, no null check. Live: exit 139, `_M_data (this=0x28)` — null `this` + field offset | `writeTo` `:~82-87`; checkout sites `:~196,~223`. Fix: guard `it->second` non-null, and either keep the connection in the map during processing or use a secondary raw-pointer registry |
| L4 | Net effect: any POST `/query` with a JSON body kills the process (curl 52 "Empty reply", exit 139) | team live repro against `build/opendb --server` |

## M. Methodology

| # | Note |
|---|------|
| M1 | B3 is **live-unverifiable** until L1–L4 are fixed — every body POST crashes before reaching `Command::validate()`. Sequence: fix L, land N1 regression test, THEN fix+verify B3 over the socket. |

## N. Reproduction guide (mechanical verification, no code-reading needed)

```bash
# N.1 K1:  git clean -xfd && make        → fails at -Wformat-truncation (Linux GCC)
#          workaround (NOT the fix): make WARNINGS="-Wall -Wextra -Wpedantic"
# N.1b K2: same clean make then fails one file later at SocketUtils.cpp:42
#          (-Werror=unused-variable, g_wsaInitialized declared outside the _WIN32
#          guard) — was masked by K1. Both must pass for "plain make succeeds".
# N.2 L1-L4:
rm -rf /tmp/opendb_data && mkdir -p /tmp/opendb_data
./build/opendb --server --uri file:///tmp/opendb_data/mydb --port 8099 --workers 4 & sleep 1
curl -v -m 5 http://localhost:8099/query -H 'Content-Type: application/json' \
  -d '{"type":"query","sql":"CREATE TABLE users(id INT PRIMARY KEY, name TEXT, age INT)"}'
#   HEAD: curl (52) Empty reply from server; server exit code 139 (SIGSEGV)
#   gdb signature: SIGSEGV in basic_string::_M_data (this=0x28) → writeTo deref of null unique_ptr
# N.2b L1 isolation: grep -n "Content-Length:" -A2 src/HttpServerIoThread.cpp → find_first_not_of(" \t", clPos + 14); literal is 15 chars
# N.3 B1 (needs L fixed):
#   CREATE TABLE users(id INT PRIMARY KEY, name TEXT, age INT) / INSERT INTO users VALUES (1,'nikhil',30)
#   SELECT * → rows[{"0":1,"1":"nikhil","2":30,"_id":1}]   (WRONG: positional keys)
#   SELECT * WHERE age > 20 → rows: []                      (WRONG: age never stored)
# N.4 four-command triage: (1) make, no overrides (2) ./build/test_runner tail → 249/249
#   (3) POST CREATE TABLE → must not be empty/curl-52 (4) INSERT(1,'x') then SELECT WHERE id=1 → row with real names
```

## N1. Test gap (root cause of K/L shipping)

Zero end-to-end coverage of the real transport: socket accept → header parse → body read → `HttpSession` → dispatcher → response write. One boot-server + POST-INSERT + SELECT-readback test would have caught L1–L4 (and later B3). HttpApi tests use `handleRequest()` directly; HttpServer tests cover lifecycle/GETs only.

---

## B. Correctness defects (code)

| # | Defect | Evidence | Severity |
|---|--------|----------|----------|
| B1 | **INSERT column list parsed then discarded**; values get positional names `"0","1",..` (`SqlParser.cpp:499-510,533-534,836`; codified in `SqlParser.test.cpp:94`); nothing maps to schema across parser→HttpApi→EngineDispatcher→both engines. `(b,a)` reorder silently mis-maps; bare `VALUES` insert breaks all named reads/WHERE/JOINs → README/USER_GUIDE examples broken | traced end-to-end | **CRITICAL** |
| B2 | **UPDATE = full-row replacement, not merge** — all three dispatch paths build `Tuple row = *cmd.values` (SET cols only) + `_id` then `put`; untouched columns destroyed (`EngineDispatcher.cpp:241-243`, `HttpApi.hpp:277-279`, `EngineLoop.hpp:321-324`). No test asserts preservation (`EngineLoop.test.cpp:80-105` checks only the SET column) | 3 sites | **CRITICAL** (or undocumented semantics deviation) |
| B3 | **begin/commit/rollback over real server crashes the process**: `HttpSession` fabricates `Command(Insert, "__begin__", values=nullopt)` (`HttpSession.hpp:82-84`) → `Command::validate()` throws (`Command.hpp:154-155`) → thrown in `nextCommand()`, **outside** the dispatcher try/catch (`EngineDispatcher.cpp:158` vs try at :194) → uncaught on jthread → `std::terminate`. Live-verify AFTER L per M1 | code-evident | **CRITICAL** |
| B4 | **Nullability never enforced** (contradicts `Schema.hpp:41` and `HttpApi.hpp:243` claims); `nullable` only serialized to disk (`LocalFileStorageProvider.hpp:606,713`) | HIGH |
| B5 | **Multi-statement requests drop statements after the first** — `stmts[0]` only (`HttpApi.hpp:148-152`, `HttpSession.hpp:86-90`) | HIGH |
| B6 | **`parseAll` swallows per-statement parse errors**, continues silently (`SqlParser.cpp:187-208`) | HIGH |
| B7 | **`parse()` never rejects trailing input** — `VALUES (1),(2)` silently drops rows 2..N (USER_GUIDE advertises multi-row) | HIGH |
| B8 | **No type enforcement vs declared column types** — schema types decorative on write path | HIGH |
| B9 | `BTree::put` returns `false` on same-(key,seq,txn) duplicate; provider ignores it — silent no-op write path | `LocalFileStorageProvider.hpp:401` | MEDIUM |
| B10 | `Date`/`Timestamp` bucket with numerics in `compare()` — Date equals Int64 of same payload as predicate AND as map/BTree key | `Value.hpp:29-31` (documented, dangerous) | MEDIUM |
| B11 | Deadlock handling = precheck only (`EngineLoop.hpp:73-92` admits race; `EngineDispatcher.cpp:172` same); real cycles resolve via 50s timeout, both sides abort | documented tradeoff | MEDIUM |

## C. Availability / failure points

| # | Point of failure | Evidence |
|---|------------------|----------|
| C1 | Dispatcher queue unbounded → memory DoS past socket gating | `EngineDispatcher.cpp:52-58` |
| C2 | JSON parser recursion, **no depth cap** → stack-overflow via nested body within the 10 MB cap | `JsonEncoder.hpp:207` |
| C3 | SQL predicate parser recursion (parens), **no depth cap** | `SqlParser.cpp` |
| C4 | Crash during `saveMetadata()` → file refuses to reopen (CRC) = total outage + manual recovery/data loss | `adr/003:34-45` |
| C5 | `HttpApiAccessPlugin::run()` busy-spins (`while(opened_){}`) | `HttpApi.hpp:89-96` |
| C6 | Uncaught-exception surface outside dispatch switch (nextCommand, Command ctor, encoders) runs unguarded on worker threads — B3 is the proven instance | `EngineDispatcher.cpp` |
| C7 | Result sets fully materialized then serialized — no streaming/backpressure on huge SELECTs | `EngineDispatcher.cpp:199-214` |
| C8 | REPL locks wait forever (blocking `acquire`; no timeout unlike dispatcher's 50s) | `EngineLoop.hpp:93` |
| C9 | `queryTimeout` is session-cumulative, boundary-checked only — single long SELECT uninterruptible (code honest; README not) | `EngineDispatcher.cpp:84-100,154` |
| C10 | **OPEN-unconfirmed.** Windows idle-exit: under `Start-Process -RedirectStandardOutput/Error` the server exits ~1 s after a *successful* listen with zero requests (2/2 via `build/smoke_idle.ps1`); not observed with direct in-console backgrounding (stays up, dies only on first body POST — the L-class signature). Linux with stdin closed stays idle-alive (independent run) — consistent with a Windows console/stdio-teardown harness artifact, not proof. Chunk-2 live verification must use the in-console pattern; characterize or reclassify there. | `build/smoke_idle.ps1` (2×) |

## D. Durability risks

| # | Risk |
|---|------|
| D1 | No WAL; commit = flush → saveMetadata → fsync with non-atomic window (C4) — plan 2.2 |
| D2 | Metadata page 1 rewritten in place per commit — single point of failure for the file |
| D3 | **No version GC**: in-memory version vectors per key grow forever (`InMemoryStorageEngine.hpp:222`); BTree never purges sub-horizon versions; **deletes never merge/rebalance pages** (no merge logic in `BTree.cpp`; only whole-page recycle 6.4). VACUUM = plan 9.3 |
| D4 | README "point-in-time backup" ≈ cold snapshots only; no incremental/PITR |

## E. Concurrency bottlenecks

| # | Bottleneck |
|---|-----------|
| E1 | **One provider mutex serializes ALL I/O** (header comment admits; per-table locks deferred) — `LocalFileStorageProvider.hpp:64-66` |
| E2 | **Row-level locks unused by production** — table-level only on all paths; `acquireKey`/`tryAcquireKey` called only from `Core.test.cpp`; README's "row-level locks" latent |
| E3 | No MVCC snapshot reads — long SELECT blocks table writers (plan 7.1) |
| E4 | Cycle walk before every lock acquisition on hot path (`EngineDispatcher.cpp:172`) |
| E5 | `select()` loop: O(nfds)/pass, FD_SETSIZE ~1024 vs maxConnections default 1024, one accept thread (adr/004 documented) |
| E6 | JOIN = nested loop + full right-table materialization per clause (`EngineLoop.hpp:141-201`) |

## F. Query-execution optimizations pending

| # | Item |
|---|------|
| F1 | **No predicate pushdown/index use** — `_id = 5` full-scans; `BTree::get` exists, SQL never calls it (3 dispatch sites) |
| F2 | ORDER BY + LIMIT = full sort; no top-N heap (`EngineLoop.hpp:242-280`) |
| F3 | No plan/prepared cache (plan 7.2) — parse+validate per request |
| F4 | No aggregates/GROUP BY/HAVING (plan 4.5/4.6) |
| F5 | Shard reads full-fan for point lookups (plan 10.1) |
| F6 | No bulk ingest (plan 7.3) |
| F7 | Per-row tuple copies on scan paths (`EngineDispatcher.cpp:203`) |
| F8 | No result streaming (C7) |

## G. Storage / Pager optimizations pending

| # | Item |
|---|------|
| G1 | **No buffer pool** — every `readPage` = fstream seek+read+CRC32 (`Pager.cpp:51-67`); BTree descents re-read pages from disk; the only cache is `CachingStorageEngine`, not wired into LocalFile |
| G2 | **`CachingStorageEngine` unbounded** — no eviction/cap; `cacheSize()` is a metric, not a limit (`CachingStorageEngine.hpp:106`) |
| G3 | fsync per commit; no group commit — throughput capped by fsync latency |
| G4 | Software table CRC32 instead of SSE4.2 hardware CRC (`Pager.cpp:142-161`; BUILD.md already requires SSE4.2) |
| G5 | fstream per-op I/O; no batched/vectored writes |
| G6 | Buddy free-run persistence cap 507 runs; overflow = safe leaks; no compaction (9.3) |
| G7 | No compression anywhere |

## H. Observability / ops gaps

| # | Gap |
|---|-----|
| H1 | `/metrics` works (`HttpServer.cpp:238-249`); slow-query log + trace IDs absent (9.1/9.2 partial); no per-table stats |
| H2 | **No logging at all** — only `std::cout/cerr` in `main.cpp` |
| H3 | Latency = log2 buckets + max/total only; no percentiles |
| H4 | Flags-only config; no file/env config |

## I. Security gaps (plan Phase 8)

| # | Gap |
|---|-----|
| I1 | No TLS (`tlsContext` declared, no load path) |
| I2 | No authn — anyone reachable can `DROP TABLE` |
| I3 | No authz/roles |
| I4 | No audit log |
| I5 | C2/C3 parse-depth DoS is also a security item |

## J. Hygiene

| # | Item |
|---|------|
| J1 | `CONTEXT.md` (the per-session briefing) is the stalest file — highest-leverage doc fix |
| J2 | `.swarm/SWARM_PLAN.md` superseded but unmarked; plan Phase-1 close-out marker left `[PENDING]` |
| J3 | Scratch files (`link.bat`, `test_compile.cpp`, …) untracked — confirm `.gitignore` permanence |

---

## A. Documentation inaccuracies (21)

| # | Location | Claim → Reality |
|---|----------|-----------------|
| A1 | `CONTEXT.md` | Last commit `55f2ca3`; 1.2 unwired; 1.8 declaration-only; 247 tests/4 fail; next-steps open → HEAD is `98106b1`; 1.2 wired (`main.cpp:155-186`); 1.8 implemented (`EngineDispatcher.cpp:88,154`); 249/249 green; steps done. **Also:** its "Linux warnings = Low" note was wrong — that's K1 |
| A2 | `docs/PHASE1-CHANGELOG.md` | Header "7 of 8; 1.8 declaration-only"; §1.2 "REPL only"; §1.8 "no caller"; config/error tables "no effect/no producer" → all false at HEAD; contradicts own footer (249/249) |
| A3 | `README.md:52,64,114-135` | "209"/"209/209"/"216" test tallies (SqlParser 34) → 249 (SqlParser 48) |
| A4 | `README.md:16-19` | "**per-statement** query timeout" → cumulative per-session, boundary-checked only (never preempts mid-statement) |
| A5 | `README.md:74-86`; `USER_GUIDE.md:43-44,127-131,274,434,548-550` | curl/SQL examples stored → named readback → **broken at HEAD** (L-series + B1 stacked) |
| A6 | `USER_GUIDE.md:43-44,548-550` | SQL typed into the REPL → REPL parses only toy `{k:v}` syntax (`main.cpp:184`, ReplSource) |
| A7 | `USER_GUIDE.md:421,427,757,775,859` | column-list/multi-row INSERT, `EXPLAIN`, `INSERT…SELECT COALESCE(MAX…)+1` → none implemented (multi-row silently drops rows 2..N — B7) |
| A8 | `BUILD.md:236-264,370` | per-suite binaries `./build/core_test`, `ed_test`, … → don't exist; Makefile builds only `opendb`+`test_runner`. "Linux fully supported" row false per K1 |
| A9 | `BUILD.md:41` | "GNU Make / Ninja 1.11+" → no Ninja support |
| A10 | `API.md:43` | "`CrashDurable` — data survives mid-write crash" → overclaims vs `adr/003` (mid-metadata crash → CRC → refuses to open → manual recovery) |
| A11 | `USER_GUIDE.md:744` | `OPENDB_LOG_LEVEL` env var → fabricated; zero `getenv` in code; no logging exists (H2) |
| A12 | `Schema.hpp:41`; `HttpApi.hpp:242-244` | "put() rejects null on non-nullable" → never enforced (B4) |
| A13 | `SqlParser.cpp:514-518` | comment: DEFAULT-VALUES marker = one-element tuple w/ `Value::null()` → code emits **empty** tuple; test agrees with code; comment lies |
| A14 | `HttpApi.hpp:60-66` | self-describes as "stub (no socket binding)" while advertising `AsyncCapable` — nothing async exists |
| A15 | `.swarm/plan.md` 9.1 | "expose the metrics snapshot" pending → `/metrics` exists (`HttpServer.cpp:238`) |
| A16 | `.swarm/plan.md` 3.1 | cites "silent fallback to in-memory" → none at HEAD (`main.cpp:95-96` hard-fails) |
| A17 | `.swarm/plan.md` 2.1 | frames CrashDurable fix as flag removal → real work = API.md gloss + crash-window test (ADR-003's scoped def + per-page CRC32 are real, `Pager.cpp:56-62`) |
| A18 | `adr/003:47` | WAL "Phase 6+ work" → superseded by plan 2.2 (amend ADR status line) |
| A19 | `.swarm/SWARM_PLAN.md` | presents as active plan → superseded by `.swarm/plan.md`; unmarked (J2) |
| A20 | `.swarm/plan.md` Phase 1 marker | text says "8/8 complete" → still `[PENDING]`/unchecked |
| A21 | `build.ps1` comment | "209/209 tests" → 249 (A3 sibling) |

---

## Verification log

- 2026-09-25 — chunk 1 (`fix/audit-k1`): K1 **confirmed fixed on Linux GCC** by independent re-audit
  (all `Value.hpp` consumers compile clean under default `-Werror`). Same run unmasked **K2**
  (fixed in the same chunk, second commit). L1–L4 re-confirmed live-failing on Linux (curl empty
  reply, exit 139) — unchanged, awaiting chunk 2. Local serial `make` clean-tree EXIT=0;
  `test_runner` 249/249 with K1+K2 in. C10 idle-exit downgraded from "resolved (harness artifact)"
  to OPEN-unconfirmed per the independent run's scope note.

## Test-suite reality

“249/249 green” is real but never covered: named-column readback after SQL `VALUES` insert (B1),
UPDATE column-preservation (B2), txn control over the server (B3), body POST over the socket (L-series, N1),
multi-statement batches (B5), trailing-input rejection (B7), nullability rejection (B4/B12-enforcement).
