# OpenDB — Merged Defect & Debt Catalog (team audit + independent audit + 2nd-pass audit)

**Ground truth:** HEAD `69cb0191a73d48da0bdfd3af76dd061f325c2baf` (`69cb019`, branch `docs/audit-merge`; this merge was performed from the clean worktree at that commit). Build clean + **249/249 tests pass** on Windows MSYS2 (GCC 14.2.0; re-confirmed by the 2nd-pass audit with a from-clean build and a runner execution). Chunk 1 (K-series) is merged: K1/K2 fixed and independently confirmed on Linux GCC 13.3; K3 primary failure fixed, residual WATCH (see K3). The **live HTTP server remains unusable**: on Linux it **crashes on the first POST with a body** (L1–L4); on Windows it **intermittently exits ~1–2 s after a successful listen, zero requests received** (C10 — reproduced 2/2 live by the 2nd-pass audit, 2026-10-08). Green suite coexists with a DOA server because no test drives a body POST through the real socket path (N1), and the suite also avoids every pattern that triggers the data-defects below (M2).

**Top-line escalation (2nd-pass + independent reviewer, VERIFIED-LIVE):** `BTree::splitLeaf` severs the leaf sibling chain (B13), and because `LocalFileStorageProvider::commit()` collects staged writes via `BTree::scanAll()` — the same severed chain — **random-order transactions silently lose most of their staged writes at commit** (B14). This is committed-data loss, not just rows missing from SELECT. Net consequence: at HEAD there is **no working path that executes SQL end-to-end** — the REPL parses only the toy `{k:v}` syntax (A6), and SQL reaches the engine only via the broken HTTP transport (L1–L4/C10).

**Fix critical path (agreed):**

```
K-series (chunk 1, merged) → chunk 2 (L1, L3a, N1 socket test, C10) → chunk 3 (B13/B14 BTree split fix, Part 2 spec) → B3 → B1 → B2 → B4/B8 → B16 (2nd-pass P1-5) → docs sweep (A-series) → WAL (Phase 2)
```

**Proposed, NOT agreed:** the 2nd-pass audit's own chunk ordering (transport → correctness → scale → ops → features) is a proposal only; the path above is the agreed one.

**OPEN DESIGN DECISION (owner):** wire `EngineDispatcher` back into the production path, or delete it. It currently has no production caller (C11). The decision affects B12, C11, B16, C12. Neither choice is agreed.

**Status legend — every finding row carries exactly one of:**

- `VERIFIED-LIVE` — reproduced by running something (server, probes, build).
- `VERIFIED-CODE` — confirmed by reading the code at the cited lines.
- `UNVERIFIED` — claimed (2nd-pass audit), NOT independently confirmed; reproduce before acting on it.

Section M rows are process notes, not findings (status not applicable there). Sections O/P record gaps-vs-vision and planned work; their status verifies the *inventory*, not severity. Section X is the 2nd-pass→catalog ID cross-reference.

B-series doc examples are blocked by TWO stacked defects: L1/L2 (body stripped → parse error) AND B1
(positional columns → empty named reads). Doc claims (A5) only become true after both land.
(Chunk-1 history: link 1 had been expanded to **K1+K2** per the independent audit — K2 was masked by K1
and blocked the same "plain `make` succeeds" acceptance criterion. Both are now merged and fixed.)

---

## K. Build & toolchain

| # | Item | Evidence | Severity | Status |
|---|------|----------|----------|--------|
| K1 | `make` with default `-Werror` fails on Linux GCC: `-Wformat-truncation` false positive on `snprintf` in `dateToIso`/`timestampToIso` (`includes/opendb/types/Value.hpp:~369,~384`, surfaces via any TU including `Value.hpp`). Undocumented workaround: `make WARNINGS="-Wall -Wextra -Wpedantic"`. Windows MSYS2 unaffected. **Do NOT fix by dropping `-Werror` globally.** Verify: plain `make` produces both binaries, 249/249 unchanged. | team live repro | HIGH — **FIXED chunk 1 (`eca7e21`), confirmed by independent Linux re-run** | VERIFIED-LIVE |
| K2 | `src/SocketUtils.cpp`: `g_wsaInitialized`/`g_wsaMu` declared unconditionally, used only inside `#if defined(_WIN32)` → `-Werror=unused-variable` on non-Windows. Found by the independent re-audit (not the original catalog); pre-existing since Phase 5 (`72e0a2a`); masked by K1 because the build aborted at `Value.hpp` first. Fix: declarations moved inside the `_WIN32` guard; POSIX keeps an inline no-op `ensureWsa()`. | independent audit (Linux GCC) | HIGH — **FIXED chunk 1 (second commit)** | VERIFIED-LIVE |
| K3 | Makefile link targets order-only-depend on `$(BUILD)`, but only the child dirs had a bootstrap rule — no rule for `build` itself. Serial masked it; `-j` failed "No rule to make target 'build'" (team: 5/5 red). **That failure mode FIXED and is now structurally impossible** (`$(BUILD)` added to the shared bootstrap rule at `Makefile:79`). Post-fix evidence, Windows (MSYS2): 28 consecutive clean from-clean `make -j8` runs, one further EXIT=2 during dir bootstrap with error text never captured (harness discarded output once; a capture batch was killed by a shell-timeout) — residual **NOT characterized**. Independent Linux results (GCC 13.3): pre-fix failure reproduced 5/5 with `rm -rf build && make -j8`; post-fix, 28 complete clean `-j8` runs, 0 failures (one further run cut off by a tool timeout, counted as neither pass nor fail). Consistent with the residual being Windows-spawn-specific (concurrent PowerShell `New-Item` processes), but **NOT proof**. Stance: **primary failure fixed, residual WATCH** — do NOT write "fixed" — pending a Windows recurrence with captured text or a much larger clean sample. K3 hunt artifacts (`tmp_k3_hunt_summary.txt`, `tmp_k3_run_*.log`, `tmp_k3_failure.txt`) searched for at merge time (2026-10-08, recursive): **not present in this worktree**; no failure text available. Do not resume the hunt as part of docs work. | team 5/5 repro; Windows 28/29; Linux 28/28 + 1 timeout | HIGH → primary failure fixed, residual **WATCH** | VERIFIED-LIVE |
| K4 (perf note) | Each directory bootstrap spawns a full `powershell.exe` process (~1 s+ each; up to 3 spawns per clean build on the MSYS2 path). Not a correctness issue; candidate: create all three dirs in one `New-Item` call or teach MSYS2 path to use `mkdir -p` (sh is already the recipe shell there). Logged for later; no action in chunk 1. | build timing observation | LOW | VERIFIED-LIVE |
| K5 | **Release-quality build flags absent** — no `-O`/`-DNDEBUG` anywhere in the Makefile; shipped binaries build at `-O0` (`Makefile:16-20,84`). AND `include-deps` is an empty no-op phony rule, so **header edits do not trigger recompiles** — stale-object hazard for incremental builds (`Makefile:131-135`). Origin: 2nd-pass P3-17. **Correction recorded:** the 2nd-pass claim that this "explains the flaky parallel-build residual" is RETRACTED as speculation — stale objects would yield a wrong binary or a link error, not a failure during directory bootstrap; this item has **no link to K3**. Practical rule: only from-clean builds count as verification. | 2nd-pass audit (code read, confirmed by independent reviewer) | HIGH (hazard) | VERIFIED-CODE |

## L. Transport layer (all CRITICAL — server dies on first body POST)

| # | Item | Evidence | Status |
|---|------|----------|--------|
| L1 | `Content-Length:` off-by-one: literal is 15 chars, parser skips `clPos + 14` → substring starts at `:` → `stoull` throws → swallowed → `contentLength = 0` for every request | `src/HttpServerIoThread.cpp:~291-292`. Fix: `std::strlen("Content-Length:")`, no magic number | VERIFIED-LIVE |
| L2 | Consequence: `totalExpected` excludes the body (`~:298`) → JSON payload stripped → downstream parse fails on every body POST | derived from L1 | VERIFIED-CODE |
| L3 | Error write-back segfaults: read loop moves `unique_ptr<Connection>` out of `conns_` during `handleRead()` but leaves the key → `IoThread::writeTo()` (called synchronously from that same stack) finds present-but-null entry and dereferences it, no null check. Live: exit 139, `_M_data (this=0x28)` — null `this` + field offset | `writeTo` `:~82-87`; checkout sites `:~196,~223`. Fix: guard `it->second` non-null, and either keep the connection in the map during processing or use a secondary raw-pointer registry | VERIFIED-LIVE |
| L4 | Net effect: any POST `/query` with a JSON body kills the process (curl 52 "Empty reply", exit 139) | team live repro against `build/opendb --server` | VERIFIED-LIVE |
| L3a | **Chunk-2 scope, NOT APPLIED (logged from independent review of the WIP).** `inflight_` is declared and read, never written — both checkout sites in `run()` move the connection out of `conns_` without registering it, so `writeTo()` finds nothing in either map and **silently drops the response**: server no longer crashes, but curl now hangs → "Empty reply" (curl 52/timeout shape). Chunk-2 fix: at both checkout sites, under the same lock, add `inflight_[fd] = c.get();` after the move and `inflight_.erase(fd);` before reinsertion/close. | team grep + live test of the WIP files | VERIFIED-CODE |
| L3b | **Chunk-2 scope, NOT APPLIED (same review).** Finding 1: the `writeTo()` null-check on the primary `conns_` lookup is correct — server no longer segfaults with it — keep it. Finding 3: L1 (`clPos + 14`) is **untouched in the WIP** — body still truncates to zero until L1 lands even after L3a is fixed. | team live test + diff read | VERIFIED-CODE |

**Chunk-2 worktree state (recorded 2026-10-08; deliberately NOT reconciled this turn):** the independent reviewer reports an uncommitted partial L3 fix in the worktree (writeTo null-check kept; `inflight_` declared and read but never written; L1 untouched), AND the same WIP also sitting in `stash@{0}` (`On master: chunk2 WIP`; confirmed to touch `src/HttpServerIoThread.{cpp,hpp}` and to add the `inflight_` map). Observed at HEAD `69cb019` during this merge: the worktree is **CLEAN** — the partial fix is reachable only via the stash. The discrepancy between the report and the observed tree is logged, not resolved.

**L-series fix recipes (recorded, NOT applied):**

- **L1:** `clPos + 14` should be `clPos + std::strlen("Content-Length:")`; the literal is 15 chars, so the parse currently starts on the colon, `stoull` throws, and `contentLength` becomes 0.
- **L3a:** in BOTH checkout sites in `IoThread::run()` (the readyReads and writeFds loops), set `inflight_[fd] = c.get()` under the same lock as the `std::move`, and `inflight_.erase(fd)` before the connection goes back into `conns_` or is destroyed.

## M. Methodology

| # | Note | Status |
|---|------|--------|
| M1 | B3 is **live-unverifiable** until L1–L4 are fixed — every body POST crashes before reaching `Command::validate()`. Sequence: fix L, land N1 regression test, THEN fix+verify B3 over the socket. | VERIFIED-CODE |

**Process lessons (2nd-pass retrospective; notes, not findings — no status applies):**

- **M2:** "249/249 green" certified none of the failing behaviors; the suite avoids them (see Test-suite reality).
- **M3:** A 1-in-N intermittent failure after a fix is not "mostly green". It may mean the window narrowed rather than closed.
- **M4:** Never pipe a build's output to Out-Null before confirming exit 0; capture to a file per run, unconditionally.
- **M5:** Verification means: build from clean with no WARNINGS override (serial AND -j8), test_runner passes, the item's own live repro passes against a running server, and the N.4 triage is rerun. Windows and Linux are both required; Linux is checked by the independent reviewer.

## N. Reproduction guide (mechanical verification, no code-reading needed)

```bash
# N.1 K1:  git clean -xfd && make        → fails at -Wformat-truncation (Linux GCC)   [FIXED chunk 1]
#          workaround (NOT the fix): make WARNINGS="-Wall -Wextra -Wpedantic"
# N.1b K2: same clean make then fails one file later at SocketUtils.cpp:42           [FIXED chunk 1]
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

## N.5. B13/B14 — BTree split severs sibling chain; commit loses staged writes (probes)

```bash
# N.5 (from repo root; probes committed at tools/probes/; chunk-3 falsifiable repro):
g++ -std=c++2b -Iincludes tools/probes/btree_probe.cpp src/BTree.cpp src/Pager.cpp src/BuddyPageAllocator.cpp -o build/btree_probe
g++ -std=c++2b -Iincludes tools/probes/commit_probe.cpp src/BTree.cpp src/Pager.cpp src/BuddyPageAllocator.cpp -o build/commit_probe
./build/btree_probe ; ./build/commit_probe
#
# ACCEPTANCE for chunk 3 (SHAPE-BASED — do NOT compare against specific
# broken numbers; those are seed/payload-dependent):
#   btree_probe : scan count == inserted count (400/400) for ascending,
#                 descending AND random orders; point-gets == inserted in all
#                 three orders; exit 0.
#   commit_probe: commit-reachable == staged (400); reader sees 400/400;
#                 exit 0.
#
# Expected output AFTER the splitLeaf fix (shape): all three insert orders
# scan=400/400 OK; commit probe: commit-reachable=400, reader sees 400/400;
# both probes exit 0.
#
# Broken output at 69cb019 (shape: descending/random scan << inserted while
# point-gets == inserted; commit-reachable << staged; both probes exit 1):
#     Independent reviewer (Linux GCC 13.3):
#       ascending  scan=400/400 OK
#       descending scan=70/400   (point-gets find all 400)
#       random     scan=84/400   (point-gets find all 400)
#       commit: 400 random-order staged writes -> 78 reachable by commit()'s
#       scanAll, 322 never committed (permanently lost).
#     2nd-pass merge re-run (Windows MSYS2 GCC 14.2.0, committed probes):
#       ascending  scan=400/400 OK   (point-gets find 400/400)
#       descending scan=64/400        (point-gets find 400/400)
#       random     scan=107/400      (point-gets find 400/400)
#       commit: staged=400 commit-reachable=107, reader sees 107/400
#       (293 permanently lost).
#   CAVEAT: any database file that already went through a non-rightmost split
#   has a damaged sibling chain ON DISK. Fixing splitLeaf will not repair
#   existing files; they need a rebuild/migration path.
```

## N1. Test gap (root cause of K/L shipping)

Zero end-to-end coverage of the real transport: socket accept → header parse → body read → `HttpSession` → dispatcher → response write. One boot-server + POST-INSERT + SELECT-readback test would have caught L1–L4 (and later B3). HttpApi tests use `handleRequest()` directly; HttpServer tests cover lifecycle/GETs only. Status: VERIFIED-CODE.

---

## B. Correctness defects (code)

| # | Defect | Evidence | Severity | Status |
|---|--------|----------|----------|--------|
| B1 | **INSERT column list parsed then discarded**; values get positional names `"0","1",..` (`SqlParser.cpp:499-510,533-534,836`; codified in `SqlParser.test.cpp:94`); nothing maps to schema across parser→HttpApi→EngineDispatcher→both engines. `(b,a)` reorder silently mis-maps; bare `VALUES` insert breaks all named reads/WHERE/JOINs → README/USER_GUIDE examples broken. *(Also 2nd-pass P1-4 — duplicate, merged.)* | traced end-to-end | **CRITICAL** | VERIFIED-CODE |
| B2 | **UPDATE = full-row replacement, not merge** — all three dispatch paths build `Tuple row = *cmd.values` (SET cols only) + `_id` then `put`; untouched columns destroyed (`EngineDispatcher.cpp:241-243`, `HttpApi.hpp:277-279`, `EngineLoop.hpp:321-324`). No test asserts preservation (`EngineLoop.test.cpp:80-105` checks only the SET column). *(Also 2nd-pass P1-3 — duplicate, merged.)* | 3 sites | **CRITICAL** (or undocumented semantics deviation) | VERIFIED-CODE |
| B3 | **begin/commit/rollback over real server crashes the process**: `HttpSession` fabricates `Command(Insert, "__begin__", values=nullopt)` (`HttpSession.hpp:82-84`) → `Command::validate()` throws (`Command.hpp:154-155`) → thrown in `nextCommand()`, **outside** the dispatcher try/catch (`EngineDispatcher.cpp:158` vs try at :194) → uncaught on jthread → `std::terminate`. Live-verify AFTER L per M1. *(Also 2nd-pass P0-3 — duplicate, merged.)* | code-evident | **CRITICAL** | VERIFIED-CODE |
| B4 | **Nullability never enforced** (contradicts `Schema.hpp:41` and `HttpApi.hpp:243` claims); `nullable` only serialized to disk (`LocalFileStorageProvider.hpp:606,713`). *(Also 2nd-pass P1-14, with B8 — duplicate, merged.)* | | HIGH | VERIFIED-CODE |
| B5 | **Multi-statement requests drop statements after the first** — `stmts[0]` only (`HttpApi.hpp:148-152`, `HttpSession.hpp:86-90`). See also B27 (DDL dropped with an empty response on the same path). | | HIGH | VERIFIED-CODE |
| B6 | **`parseAll` swallows per-statement parse errors**, continues silently (`SqlParser.cpp:187-208`). See also B23 (escape-blind splitter). | | HIGH | VERIFIED-CODE |
| B7 | **`parse()` never rejects trailing input** — `VALUES (1),(2)` silently drops rows 2..N (USER_GUIDE advertises multi-row). | | HIGH | VERIFIED-CODE |
| B8 | **No type enforcement vs declared column types** — schema types decorative on write path. *(Also 2nd-pass P1-14, with B4 — duplicate, merged.)* | | HIGH | VERIFIED-CODE |
| B9 | `BTree::put` returns `false` on same-(key,seq,txn) duplicate; provider ignores it — silent no-op write path. *(Also 2nd-pass P1-15 — duplicate, merged.)* | `LocalFileStorageProvider.hpp:401` | MEDIUM | VERIFIED-CODE |
| B10 | `Date`/`Timestamp` bucket with numerics in `compare()` — Date equals Int64 of same payload as predicate AND as map/BTree key | `Value.hpp:29-31` (documented, dangerous) | MEDIUM | VERIFIED-CODE |
| B11 | Deadlock handling = precheck only (`EngineLoop.hpp:73-92` admits race; `EngineDispatcher.cpp:172` same); real cycles resolve via 50s timeout, both sides abort. **2nd-pass P1-11 addendum (UNVERIFIED):** the precheck is worse than "racy" — `detectCycle(txn)` is called *before* the txn enters the wait queue, so `waiterTable(origin)` is empty at check time and the precheck is inert even for its intended role. | documented tradeoff | MEDIUM | VERIFIED-CODE (precheck-only); addendum UNVERIFIED |
| B12 | **Production execution paths ignore JOIN / ORDER BY / LIMIT / OFFSET**: `HttpApi::handleDml` and `EngineDispatcher::runSession` re-implement the executor without them; only the REPL's `EngineLoop` honors them. README's HTTP examples (`ORDER BY … LIMIT 10`, JOIN over HTTP) silently return unsorted, unlimited, unjoined rows. Origin: 2nd-pass P0-4. | zero references to `joins`/`orderBy`/`limit`/`offset` in `includes/opendb/frontend/HttpApi.hpp:209-306` and `src/EngineDispatcher.cpp:195-270`, vs `includes/opendb/core/EngineLoop.hpp:108-280` | **CRITICAL** (silent contract violation) | VERIFIED-CODE |
| B13 | **`BTree::splitLeaf` severs the leaf sibling chain**: the new right page is written with `right_sibling = 0` instead of the split leaf's old sibling (`NodeHeader rhdr{…, 0}`); the old sibling value is never passed in. Any split of a non-rightmost leaf orphans every page to its right — `scan()`/`scanAll()` silently lose those rows while point `get()` still finds them via the internal nodes. The unit suite stays green because `BTree_Put_Many_Rows_Triggers_Splits` inserts ascending keys only — ascending always splits the rightmost leaf, where the bug is invisible. Origin: 2nd-pass P1-1. Repro: `tools/probes/btree_probe.cpp` (N.5). | `src/BTree.cpp:398-417` (rhdr at :415); `tests/BTree.test.cpp:182-209` | **CRITICAL** | VERIFIED-LIVE |
| B14 | **Committed-data loss via the severed chain** (escalation of B13, independent reviewer): `LocalFileStorageProvider::commit()` collects a txn's staged writes with `BTree::scanAll()` — the same severed chain — so staged writes on orphaned pages are never promoted; they stay at `commitSeq==0`, visible only to a finished transaction, and are **permanently lost**. Live numbers in N.5 (reviewer/Linux: 400 random-order staged writes → 78 reachable, 322 never committed; 2nd-pass re-run/Windows with the committed probes: 107 reachable, 293 lost). On-disk caveat: files that already went through a non-rightmost split carry the damaged chain **on disk**; the code fix does not repair existing files. Origin: independent reviewer (Linux GCC 13.3) + 2nd-pass probe re-run. | `LocalFileStorageProvider.hpp:451-463` (scanAll-based commit) walking `src/BTree.cpp:516-538`; repro `tools/probes/commit_probe.cpp` (N.5) | **CRITICAL** (worse than B13: committed-data loss) | VERIFIED-LIVE |
| B15 | **MVCC version groups can straddle a page split**: `(key, commitSeq DESC)` ordering lets one key's versions land on both sides of a split; `get()` scans only the routed leaf, so a key whose newest version is invisible (staged by another txn) can miss an older *visible* version on the left page — row silently missing; `scan()` can also report a key twice (once per page). Origin: 2nd-pass P1-2. **REPRODUCED (chunk 3, 2026-10-09)** via `tools/probes/mvcc_split_probe.cpp`: 31 left keys + key 500 with one committed (seq=5) and one staged-by-other (seq=0) version straddling the count-cap split (65 entries, mid=32); the promoted separator is 500, so `get(500)` routes to the right page holding only the invisible staged version → NOT FOUND, while `scan()` sees the visible (500,5) on the left page (probe exit 1; reproduced with the B13 fix already in — B15 is independent of the sibling chain). NOT fixed in chunk 3 (reproduction only, per chunk scope). | `src/BTree.cpp:210-244` (get assumes per-key contiguity), `:253-274` (split position), `:453-512` (per-page group scan); repro `tools/probes/mvcc_split_probe.cpp` | HIGH | VERIFIED-LIVE |
| B16 | **Dispatcher auto-commits after a failed statement**: `EngineDispatcher::runSession` presents the error, then unconditionally runs the auto-commit block (`commitTxn` + `storage_->engine()->commit()`) — a multi-row UPDATE that fails midway **commits its partial writes**. EngineLoop aborts on error; the dispatcher path does not. Origin: 2nd-pass P1-5. | `src/EngineDispatcher.cpp:276-289` (auto-commit runs regardless of `errorOpt`) | **CRITICAL** (atomicity violation) | VERIFIED-CODE |
| B17 | **`LocalFileStorageProvider::abort()` is a no-op** — its own comment admits staged entries are never removed or marked; aborted writes stay in the BTree forever, are re-walked by every future commit's `scanAll` (see E7, B14), and are never collected. Origin: 2nd-pass P1-6. | `LocalFileStorageProvider.hpp:481-496` | HIGH | VERIFIED-CODE |
| B18 | **`saveMetadata()` has no bounds checks** — unbounded table/column/extension data serialized into one fixed 4088-byte page via raw `memcpy` → heap buffer overflow (silent memory corruption) with dozens of tables or a few hundred columns. `loadMetadata` is bounds-checked; the writer is not. Origin: 2nd-pass P1-7. Cross-ref C4 (crash during save → refuses reopen; distinct failure of the same single-page design). | `LocalFileStorageProvider.hpp:562-649` | **CRITICAL** (memory safety) | VERIFIED-CODE |
| B19 | **In-memory `DROP TABLE` erases only the schema registry entry, not the row data** — dropped rows survive and reappear via scan / re-`CREATE TABLE` with the same name. Origin: 2nd-pass P1-8. | `InMemoryStorageProvider.hpp:108-116` (only `schemas_.erase`); engine `tables_` untouched | HIGH | VERIFIED-CODE |
| B20 | **Shard routing is self-inconsistent**: `put` routes by partition policy (column value in the row), `get`/`remove` route by `_id` key hash — on any partitioned table where the partition column ≠ `_id`, DELETE writes the tombstone to the wrong shard and point SELECT misses the row. Origin: 2nd-pass P1-9. | `ShardedStorageEngine.hpp:40-76` | HIGH | UNVERIFIED |
| B21 | **Lock upgrade corrupts the holder set**: a Shared holder requesting Exclusive is instantly granted (`grantableUnlocked` "self-held: always grantable") and `grantUnlocked` flips the resource to Exclusive **while other txns still hold Shared on it** — mutual exclusion broken. Origin: 2nd-pass P1-10. | `LockManager.hpp:269-306` | HIGH | VERIFIED-CODE |
| B22 | **`visibleSeq` race**: two threads committing concurrently both read `txnm_.visibleSeq()` after their `commitTxn` calls and can stamp identical commit sequences — MVCC sequence uniqueness violated. Origin: 2nd-pass P1-12. | `EngineDispatcher.cpp:283-285`, `TransactionManager.hpp:49-56` | MEDIUM | UNVERIFIED |
| B23 | **`parseAll` splits statements on `;` without honoring `\'` escapes or comments** (the lexer handles both; the splitter handles neither) — semicolons inside escaped strings/comments split statements. Sub-note: integer literals parse via `strtoll` with no errno check (silent overflow wrap). Cross-ref B6 (swallowed errors, same function) and B7 (trailing input). Origin: 2nd-pass P1-13 (new part; the rest merged into B6/B7). | `src/SqlParser.cpp:187-208` (splitter), `:103-121` (lexer escape handling), `:159-169` (strtoll) | HIGH | UNVERIFIED |
| B24 | **DDL bypasses transactions and locks entirely** (CREATE/DROP/ALTER executed straight on the provider from the front-end, auto-committed regardless of an explicit txn), and SQL-level `COMMIT`/`ROLLBACK` over HTTP is re-dispatched without a txnId so it always fails. Origin: 2nd-pass P1-16. | `HttpApi.hpp:157-175` | HIGH | UNVERIFIED |
| B25 | **HttpApi auto-commit path releases the table lock BEFORE running `storage_->engine()->commit()`** — another txn can acquire the lock and read pre-commit state in the window (isolation violation). Origin: 2nd-pass P1-17. | `HttpApi.hpp:308-317` | MEDIUM | UNVERIFIED |
| B26 | **`Value::fromBytes` has no bounds checks**: Text/Blob length fields are trusted and `std::string s(iter, iter + len)` reads past the page buffer; the only guard compares against a default-constructed (singular) iterator, itself UB. Called on every page load — one corrupt page → OOB read or crash. Origin: 2nd-pass S-1. | `includes/opendb/types/Value.hpp:290-340` | HIGH (memory safety) | UNVERIFIED |
| B27 | **DDL sent over the real server is silently dropped and returns an empty response**: `HttpSession::convertRequestToCommand` returns `nullopt` for non-DML statements → session EOF with no `present()` — CREATE TABLE via HTTP does nothing, reports nothing. Cross-ref B5. Origin: 2nd-pass S-8. | `HttpSession.hpp:85-93` | HIGH | UNVERIFIED |
| B28 | **Rows > ~4 KB crash the engine** (`BTree::saveNode` throws on leaf overflow; no overflow pages) despite the `BlobSupport` capability claim, and Text/Blob values > 4 GB silently truncate their own length prefix (`uint32_t` cast). Origin: 2nd-pass P3-10. Ties to O6 (capability overclaim). | `src/BTree.cpp:170-172`, `Value.hpp:272,279` | HIGH | UNVERIFIED |

**B1 fix note (recorded, NOT applied; from a reviewer scratch-tree prototype — never verified end-to-end, never applied to the repo):** thread the parsed INSERT column list into `parseInsertValueList`, and re-bind all-positional keys ("0","1",…) to the table's schema column order via `describeTable` in the executor. Open cases: partial column lists, reordered column lists, DEFAULT materialization, and the fact that the rebind is needed in **all three executors** (EngineLoop, EngineDispatcher, HttpApi), not just HttpApi.

## C. Availability / failure points

| # | Point of failure | Evidence | Status |
|---|------------------|----------|--------|
| C1 | Dispatcher queue unbounded → memory DoS past socket gating | `EngineDispatcher.cpp:52-58` | VERIFIED-CODE |
| C2 | JSON parser recursion, **no depth cap** → stack-overflow via nested body within the 10 MB cap. *(Also 2nd-pass S-4 — duplicate, merged.)* | `JsonEncoder.hpp:207` | VERIFIED-CODE |
| C3 | SQL predicate parser recursion (parens), **no depth cap**. **2nd-pass note:** parseOrExpr/parseAndExpr **iterate** (`SqlParser.cpp:749-775`) and parentheses are not supported at all, so no unbounded recursion path was found — this item may be moot; confirm before scheduling any work. | `SqlParser.cpp` | UNVERIFIED |
| C4 | Crash during `saveMetadata()` → file refuses to reopen (CRC) = total outage + manual recovery/data loss. Cross-ref B18 (the same single-page metadata design also overflows unboundedly on write). | `adr/003:34-45` | VERIFIED-CODE |
| C5 | `HttpApiAccessPlugin::run()` busy-spins (`while(opened_){}`) | `HttpApi.hpp:89-96` | VERIFIED-CODE |
| C6 | Uncaught-exception surface outside dispatch switch (nextCommand, Command ctor, encoders) runs unguarded on worker threads — B3 is the proven instance. **2nd-pass S-2 addendum (UNVERIFIED):** the REAL production path compounds this — `HttpApi::handleDml` has no try/catch and runs on the io thread, so storage exceptions (CRC failure, `saveNode` overflow, unknown tag) terminate the whole process. | `EngineDispatcher.cpp` | VERIFIED-CODE; addendum UNVERIFIED |
| C7 | Result sets fully materialized then serialized — no streaming/backpressure on huge SELECTs. *(Also 2nd-pass P3-7 — duplicate, merged; see F8.)* | `EngineDispatcher.cpp:199-214` | VERIFIED-CODE |
| C8 | REPL locks wait forever (blocking `acquire`; no timeout unlike dispatcher's 50s) | `EngineLoop.hpp:93` | VERIFIED-CODE |
| C9 | `queryTimeout` is session-cumulative, boundary-checked only — single long SELECT uninterruptible (code honest; README not) | `EngineDispatcher.cpp:84-100,154` | VERIFIED-CODE |
| C10 | **CONFIRMED Windows startup-stability defect** (merged 2026-10-08 from two contradictory earlier rows — one "confirmed defect", one "OPEN-unconfirmed / harness artifact"; the in-console repro plus 2nd-pass live re-runs supersede the unconfirmed framing). The server intermittently exits ~1–2 s after a *successful* listen with **zero requests**: clean `bye` print (main() returned, i.e. `http.join()` returned), no captured exit code/error. Evidence: earlier session — died pre-request in 4 of 5 live attempts across BOTH console patterns (2× redirected-stdio, 1× in-console background, 2× in triage), plus 1× died mid-POST (L-class signature); 2nd-pass merge session (Windows MSYS2 GCC 14.2.0, HEAD 69cb019, redirected-stdio): **2/2 runs exited pre-request** (clean `bye`, exit 0, port never accepting). Linux with stdin closed stays idle-alive and serves requests (team + independent reviewer). Trigger unidentified — needs instrumentation of `HttpServer::listen`/`join` and the io-loop break paths (`HttpServerIoThread::run` `rc < 0` path). **Blocks Windows-side chunk-2 live verification until characterized; Linux verification unaffected.** | earlier `build/smoke_idle.ps1` 2×, `tmp_triage.ps1` 2×, smoke runs; 2nd-pass live runs 2026-10-08 | VERIFIED-LIVE |
| C11 | **Production path bypasses `EngineDispatcher`**: `HttpServer::handleQuery` constructs an `HttpApiAccessPlugin` per request and executes SQL **synchronously on the io thread**; `dispatcher_->enqueue()` has no production caller — the worker pool and its G.1 session leak-guard / K.1 query timeout never run in production. Origin: 2nd-pass P0-5. Disposition (wire the dispatcher back in vs delete it) is an **OPEN DESIGN DECISION** (owner) affecting B12/C11/B16/C12. | `src/HttpServer.cpp:286-301`; zero `enqueue` call sites outside tests | VERIFIED-CODE |
| C12 | **Sessions pin io threads**: with queries executing inline (C11), one slow query freezes every other connection owned by that io thread (TCP reads stall), and a long query on the acceptor thread (io_impls_[0]) blocks all new accepts. Origin: 2nd-pass P3-12. | `src/HttpServerIoThread.cpp:190-208` + `HttpServer.cpp:286-301` | UNVERIFIED |
| C13 | **Unbounded request-body memory**: `Content-Length` is trusted (`stoull` of attacker-controlled text) and the io thread reads unbounded bytes into `readBuffer` before any limit is applied — the 413 check in `HttpServer::onClientRequest` runs AFTER the body is fully buffered (its "before allocation to prevent OOM" comment is false). `Content-Length: 999999999999` → OOM DoS once L1 is fixed. Origin: 2nd-pass S-3. | `src/HttpServerIoThread.cpp:288-299`, `src/HttpServer.cpp:204-211` | UNVERIFIED |
| C14 | **Slowloris**: every received byte resets the 5 s idle timer, so 1024 trickling connections (≤1 byte per 4–5 s) permanently consume all connection slots. Origin: 2nd-pass S-5. | `src/HttpServerIoThread.cpp:261-262` (lastActivity per read), `:231-241` | UNVERIFIED |
| C15 | **SQL errors are returned as HTTP 200** with a `{success:false}` body — breaks monitoring, WAFs, and client retry semantics. Origin: 2nd-pass S-9. | `src/HttpServer.cpp:296-299` | UNVERIFIED |
| C16 | **No keep-alive**: every response carries `Connection: close` (one TCP connection per query — port churn at scale), and `sendResponse`/`sendError` hand the response to EVERY io thread's `writeTo` (mutex + lookup per thread; only the owning thread acts). Origin: 2nd-pass P3-11. | `src/HttpServer.cpp:28`, `:265-280` | UNVERIFIED |
| C17 | **`TransactionManager::states_` map grows forever** — every txn ever begun stays in the map; no pruning exists despite the code comment mentioning pruning. Unbounded memory per txn. Origin: 2nd-pass P3-13. | `TransactionManager.hpp:83-86` | UNVERIFIED |

## D. Durability risks

| # | Risk | Status |
|---|------|--------|
| D1 | No WAL; commit = flush → saveMetadata → fsync with non-atomic window (C4) — plan 2.2. *(Also 2nd-pass P3-15, with D2 — duplicate, merged.)* | VERIFIED-CODE |
| D2 | Metadata page 1 rewritten in place per commit — single point of failure for the file. *(Also 2nd-pass P3-15, with D1 — duplicate, merged.)* | VERIFIED-CODE |
| D3 | **No version GC**: in-memory version vectors per key grow forever (`InMemoryStorageEngine.hpp:222`); BTree never purges sub-horizon versions; **deletes never merge/rebalance pages** (no merge logic in `BTree.cpp`; only whole-page recycle 6.4). VACUUM = plan 9.3. *(Also 2nd-pass P3-8 — duplicate, merged; B17/B14 make the dead-space debt worse: aborted writes are never collected and every commit re-walks them.)* | VERIFIED-CODE |
| D4 | README "point-in-time backup" ≈ cold snapshots only; no incremental/PITR. **2nd-pass P3-14 addendum (UNVERIFIED):** `backupTo` is a stop-the-world full file copy **under the provider mutex** — a full write blackout at every backup. | VERIFIED-CODE; addendum UNVERIFIED |
| D5 | **Cross-shard commit is non-atomic**: `ShardedStorageEngine::commit` loops children with early return — a shard-2 failure after shard-0 commit leaves a permanently partial transaction; `prepare()` is a no-op stub in every engine, so the two-phase contract is theater. No shard failure handling, rebalancing, or online add/remove. Origin: 2nd-pass P3-16. | UNVERIFIED |

## E. Concurrency bottlenecks

| # | Bottleneck | Status |
|---|-----------|--------|
| E1 | **One provider mutex serializes ALL I/O** (header comment admits; per-table locks deferred) — `LocalFileStorageProvider.hpp:64-66`. **2nd-pass P3-4 addendum (UNVERIFIED):** the mutex is also held while `scan` emits rows to the caller's callback — predicate evaluation and materialization run under the global lock. | VERIFIED-CODE; addendum UNVERIFIED |
| E2 | **Row-level locks unused by production** — table-level only on all paths; `acquireKey`/`tryAcquireKey` called only from `Core.test.cpp`; README's "row-level locks" latent | VERIFIED-CODE |
| E3 | No MVCC snapshot reads — long SELECT blocks table writers (plan 7.1) | VERIFIED-CODE |
| E4 | Cycle walk before every lock acquisition on hot path (`EngineDispatcher.cpp:172`) — and see the B11 addendum: the walk is inert at the point it is called | VERIFIED-CODE |
| E5 | `select()` loop: O(nfds)/pass, FD_SETSIZE ~1024 vs maxConnections default 1024, one accept thread (adr/004 documented). *(Also 2nd-pass S-6 — duplicate, merged: no `FD_SETSIZE` guard anywhere; OOB write into `fd_set` once fds reach 1024 on POSIX.)* | VERIFIED-CODE |
| E6 | JOIN = nested loop + full right-table materialization per clause (`EngineLoop.hpp:141-201`) | VERIFIED-CODE |
| E7 | **Every commit scans the entire database**: `LocalFileStorageProvider::commit()` runs `scanAll()` over every entry of every table (deserializing every tuple) to find the txn's staged writes; `InMemoryStorageEngine::commit` walks every row map the same way. Commit cost is O(total DB size), grows monotonically with every aborted txn (B17), and this same walk is what turns B13 into committed-data loss (B14). Origin: 2nd-pass P3-1. | VERIFIED-CODE |
| E8 | Single global condition variable with `notify_all` on every release — thundering herd: every waiter on every resource wakes for every grant. Origin: 2nd-pass P3-4 (part; the rest merged into E1). | UNVERIFIED |

## F. Query-execution optimizations pending

| # | Item | Status |
|---|------|--------|
| F1 | **No predicate pushdown/index use** — `_id = 5` full-scans; `BTree::get` exists, SQL never calls it (3 dispatch sites). *(Also 2nd-pass P3-6 — duplicate, merged; no secondary indexes exist at all — see O4.)* | VERIFIED-CODE |
| F2 | ORDER BY + LIMIT = full sort; no top-N heap (`EngineLoop.hpp:242-280`) | VERIFIED-CODE |
| F3 | No plan/prepared cache (plan 7.2) — parse+validate per request | VERIFIED-CODE |
| F4 | No aggregates/GROUP BY/HAVING (plan 4.5/4.6) | VERIFIED-CODE |
| F5 | Shard reads full-fan for point lookups (plan 10.1) | VERIFIED-CODE |
| F6 | No bulk ingest (plan 7.3) | VERIFIED-CODE |
| F7 | Per-row tuple copies on scan paths (`EngineDispatcher.cpp:203`) | VERIFIED-CODE |
| F8 | No result streaming (C7) | VERIFIED-CODE |
| F9 | **Per-row storage overhead is severe**: every row is a `Tuple` = vector<ColumnValue> **plus a per-row hash map (string→index)**, with column-name strings duplicated per value — multi-× memory amplification vs compact row layouts at scale. Origin: 2nd-pass P3-9. | UNVERIFIED |

## G. Storage / Pager optimizations pending

| # | Item | Status |
|---|------|--------|
| G1 | **No buffer pool** — every `readPage` = fstream seek+read+CRC32 (`Pager.cpp:51-67`); BTree descents re-read pages from disk; the only cache is `CachingStorageEngine`, not wired into LocalFile. *(Also 2nd-pass P3-2 — duplicate, merged.)* | VERIFIED-CODE |
| G2 | **`CachingStorageEngine` unbounded** — no eviction/cap; `cacheSize()` is a metric, not a limit (`CachingStorageEngine.hpp:106`). **2nd-pass addendum (UNVERIFIED):** it is also never wired into any production path — examples/tests only. | VERIFIED-CODE; addendum UNVERIFIED |
| G3 | fsync per commit; no group commit — throughput capped by fsync latency. **2nd-pass P3-5 addendum (UNVERIFIED):** the fsync runs under the provider mutex → global single-writer. | VERIFIED-CODE; addendum UNVERIFIED |
| G4 | Software table CRC32 instead of SSE4.2 hardware CRC (`Pager.cpp:142-161`; BUILD.md already requires SSE4.2) | VERIFIED-CODE |
| G5 | fstream per-op I/O; no batched/vectored writes | VERIFIED-CODE |
| G6 | Buddy free-run persistence cap 507 runs; overflow = safe leaks; no compaction (9.3) | VERIFIED-CODE |
| G7 | No compression anywhere | VERIFIED-CODE |
| G8 | **`BTree::insertIntoParent` walks the tree from the root on every split** to find the parent (no parent pointers, no node cache) — O(height × node loads) extra I/O per split; splits also re-encode whole pages. Origin: 2nd-pass P3-3. | UNVERIFIED |

## H. Observability / ops gaps

| # | Gap | Status |
|---|-----|--------|
| H1 | `/metrics` works (`HttpServer.cpp:238-249`); slow-query log + trace IDs absent (9.1/9.2 partial); no per-table stats | VERIFIED-CODE |
| H2 | **No logging at all** — only `std::cout/cerr` in `main.cpp` | VERIFIED-CODE |
| H3 | Latency = log2 buckets + max/total only; no percentiles | VERIFIED-CODE |
| H4 | Flags-only config; no file/env config | VERIFIED-CODE |
| H5 | **`/status` always reports 0 connections**: `stats_.connectionsActive` is never incremented (a separate `activeConnections_` counter is used), so the endpoint's connections figure is permanently zero. Origin: 2nd-pass P0-6. | UNVERIFIED |

## I. Security gaps (plan Phase 8)

| # | Gap | Status |
|---|-----|--------|
| I1 | No TLS (`tlsContext` declared, no load path). *(2nd-pass S-7 maps here and to I2–I4 — duplicate, merged.)* | VERIFIED-CODE |
| I2 | No authn — anyone reachable can `DROP TABLE` | VERIFIED-CODE |
| I3 | No authz/roles | VERIFIED-CODE |
| I4 | No audit log | VERIFIED-CODE |
| I5 | C2/C3 parse-depth DoS is also a security item | VERIFIED-CODE (C2) / UNVERIFIED (C3) |

## J. Hygiene

| # | Item | Status |
|---|------|--------|
| J1 | `CONTEXT.md` (the per-session briefing) is the stalest file — highest-leverage doc fix | VERIFIED-CODE |
| J2 | `.swarm/SWARM_PLAN.md` superseded but unmarked; plan Phase-1 close-out marker left `[PENDING]` | VERIFIED-CODE |
| J3 | Scratch files (`link.bat`, `test_compile.cpp`, …) untracked — confirm `.gitignore` permanence | VERIFIED-CODE |

---

## O. Product-vision gaps (vs. target vision — NOT defects)

Owner's target: a **SQLite-style embeddable library** with an **ODBC-style API**, **hot-swappable storage backends**, and production-grade features. This section records where the current tree stands against that vision. Status verifies the inventory (code-searched by the independent reviewer), not severity.

| # | Gap | Status |
|---|-----|--------|
| O1 | **No `extern "C"` anywhere, no shared-library build target, no in-process call API** — the only entry points are the text REPL and the (broken) HTTP server. No ODBC-style driver model. | VERIFIED-CODE |
| O2 | **Backend selection is a hardcoded URI if/else** in `main.cpp` `makeProvider()`; `main.cpp` itself says "No dlopen-style dynamic loading — no consumer in v0.1". Swappable at compile time and launch time only, not at runtime. | VERIFIED-CODE |
| O3 | **Multi-level sharding is permitted by the types** (`ShardedStorageProvider` takes `IStorageProvider` children) **but is never constructed, wired to a URI, or tested**. | VERIFIED-CODE |
| O4 | **No foreign keys** (no FOREIGN KEY/REFERENCES anywhere). No CREATE SCHEMA or namespacing ("schema" means one table's column list). Constraints: single-column PRIMARY KEY only, nullable flag only (and B4: not enforced); no UNIQUE, CHECK, composite keys. No queryable catalog (`ColumnDef.extensions` is a free-form string map, used for the auto-increment seed). | VERIFIED-CODE |
| O5 | **ALTER TABLE is AddColumn/DropColumn/RenameTable only** (`AlterSpec` in `Schema.hpp`). No migration framework: every "migrat" hit in the code is the Pager v1→v2 page-format upgrade; no versioned up/down scripts, history table, or rollback chain. | VERIFIED-CODE |
| O6 | **`LocalFileStorageProvider::capabilities()` advertises `StorageCapability::CrashDurable` while its own comments say "no WAL yet"** — the capability flag overclaims. Tie to A10 / D1 / (2nd-pass P3-15 → D1/D2). | VERIFIED-CODE |
| O7 | **Strongest area:** `LockManager`, `DeadlockDetector` and MVCC visibility are real and tested — but see B21 (2nd-pass P1-10), the B11 addendum (2nd-pass P1-11), and B22 (2nd-pass P1-12) for defects inside them. | VERIFIED-CODE |

## P. Planned structural work (platform split) — future chunk

| # | Item | Status |
|---|------|--------|
| P1 | **Platform split chunk (planned; not started).** A grep for `_WIN32`, `WSAEWOULDBLOCK`, `WSAGetLastError` and `Windows_NT` finds platform-conditional code in 7 files: `Makefile` (4 ifeq blocks), `src/SocketUtils.cpp`, `includes/opendb/frontend/SocketUtils.hpp`, `includes/opendb/frontend/HttpServer.hpp` (the `SocketHandle` type alias, used widely), `src/HttpServerIoThread.cpp` / `.hpp`, and `includes/opendb/storage/Pager.hpp` (fsync path — **highest stakes**). Scope notes: it is **NOT** a fix for K3 (that lives in the Makefile recipe); it must be its own chunk with its own plan and acceptance criteria; it starts only after the K-series and chunks 2–3 are done. | VERIFIED-CODE (inventory) |

## A. Documentation inaccuracies (21)

| # | Location | Claim → Reality | Status |
|---|----------|-----------------|--------|
| A1 | `CONTEXT.md` | Last commit `55f2ca3`; 1.2 unwired; 1.8 declaration-only; 247 tests/4 fail; next-steps open → HEAD is `98106b1`; 1.2 wired (`main.cpp:155-186`); 1.8 implemented (`EngineDispatcher.cpp:88,154`); 249/249 green; steps done. **Also:** its "Linux warnings = Low" note was wrong — that's K1 | VERIFIED-CODE |
| A2 | `docs/PHASE1-CHANGELOG.md` | Header "7 of 8; 1.8 declaration-only"; §1.2 "REPL only"; §1.8 "no caller"; config/error tables "no effect/no producer" → all false at HEAD; contradicts own footer (249/249) | VERIFIED-CODE |
| A3 | `README.md:52,64,114-135` | "209"/"209/209"/"216" test tallies (SqlParser 34) → 249 (SqlParser 48). 2nd-pass recount: 249 `TEST()` macros confirmed (per-suite: SqlParser 48, LocalFile 28, Core 26, Predicate 19, JsonEncoder 19, Pager 16, Sharded 16, BTree 14, EngineLoop 14, Value 12, Storage 7, HttpApi 7, HttpServer 6, Tuple 6, EngineDispatcher 5, Caching 3, Stress 3) — the README per-suite table is wrong in 12 of 17 rows | VERIFIED-CODE |
| A4 | `README.md:16-19` | "**per-statement** query timeout" → cumulative per-session, boundary-checked only (never preempts mid-statement) | VERIFIED-CODE |
| A5 | `README.md:74-86`; `USER_GUIDE.md:43-44,127-131,274,434,548-550` | curl/SQL examples stored → named readback → **broken at HEAD** (L-series + B1 stacked); additionally the HTTP executor ignores ORDER BY/LIMIT/JOIN (B12) even once L+B1 land | VERIFIED-CODE |
| A6 | `USER_GUIDE.md:43-44,548-550` | SQL typed into the REPL → REPL parses only toy `{k:v}` syntax (`main.cpp:184`, ReplSource). **2nd-pass synthesis (P0-2):** combined with L1–L4/C10 this means at HEAD there is **no working path that executes SQL end-to-end** | VERIFIED-CODE |
| A7 | `USER_GUIDE.md:421,427,757,775,859` | column-list/multi-row INSERT, `EXPLAIN`, `INSERT…SELECT COALESCE(MAX…)+1` → none implemented (multi-row silently drops rows 2..N — B7) | VERIFIED-CODE |
| A8 | `BUILD.md:236-264,370` | per-suite binaries `./build/core_test`, `ed_test`, … → don't exist; Makefile builds only `opendb`+`test_runner`. "Linux fully supported" row false per K1 (now fixed) — re-verify after chunk 1 | VERIFIED-CODE |
| A9 | `BUILD.md:41` | "GNU Make / Ninja 1.11+" → no Ninja support | VERIFIED-CODE |
| A10 | `API.md:43` | "`CrashDurable` — data survives mid-write crash" → overclaims vs `adr/003` (mid-metadata crash → CRC → refuses to open → manual recovery). See O6 (the capability flag itself overclaims) | VERIFIED-CODE |
| A11 | `USER_GUIDE.md:744` | `OPENDB_LOG_LEVEL` env var → fabricated; zero `getenv` in code; no logging exists (H2) | VERIFIED-CODE |
| A12 | `Schema.hpp:41`; `HttpApi.hpp:242-244` | "put() rejects null on non-nullable" → never enforced (B4/B12-enforcement) | VERIFIED-CODE |
| A13 | `SqlParser.cpp:514-518` | comment: DEFAULT-VALUES marker = one-element tuple w/ `Value::null()` → code emits **empty** tuple; test agrees with code; comment lies | VERIFIED-CODE |
| A14 | `HttpApi.hpp:60-66` | self-describes as "stub (no socket binding in v0.1)" while advertising `AsyncCapable` — nothing async exists | VERIFIED-CODE |
| A15 | `.swarm/plan.md` 9.1 | "expose the metrics snapshot" pending → `/metrics` exists (`HttpServer.cpp:238`) | VERIFIED-CODE |
| A16 | `.swarm/plan.md` 3.1 | cites "silent fallback to in-memory" → none at HEAD (`main.cpp:95-96` hard-fails) | VERIFIED-CODE |
| A17 | `.swarm/plan.md` 2.1 | frames CrashDurable fix as flag removal → real work = API.md gloss + crash-window test (ADR-003's scoped def + per-page CRC32 are real, `Pager.cpp:56-62`) | VERIFIED-CODE |
| A18 | `adr/003:47` | WAL "Phase 6+ work" → superseded by plan 2.2 (amend ADR status line) | VERIFIED-CODE |
| A19 | `.swarm/SWARM_PLAN.md` | presents as active plan → superseded by `.swarm/plan.md`; unmarked (J2) | VERIFIED-CODE |
| A20 | `.swarm/plan.md` Phase 1 marker | text says "8/8 complete" → still `[PENDING]`/unchecked | VERIFIED-CODE |
| A21 | `build.ps1` comment | "209/209 tests" → 249 (A3 sibling) | VERIFIED-CODE |

---

## X. Cross-reference: 2nd-pass IDs → catalog IDs

| 2nd-pass ID | Catalog ID(s) | Disposition |
|---|---|---|
| P0-1 | L1–L4, C10 | aggregate synthesis; no new row (header) |
| P0-2 | A6 + L1–L4/C10 | synthesis; A6 addendum; no new row |
| P0-3 | B3 | merged (duplicate) |
| P0-4 | B12 | new row |
| P0-5 | C11 | new row |
| P0-6 | H5 | new row |
| P1-1 | B13 (+B14 escalation) | new row(s) |
| P1-2 | B15 | new row |
| P1-3 | B2 | merged (duplicate) |
| P1-4 | B1 | merged (duplicate) |
| P1-5 | B16 | new row |
| P1-6 | B17 | new row |
| P1-7 | B18 | new row |
| P1-8 | B19 | new row |
| P1-9 | B20 | new row |
| P1-10 | B21 | new row |
| P1-11 | B11 | addendum (beyond-B11 part UNVERIFIED) |
| P1-12 | B22 | new row |
| P1-13 | B6 + B7 (cross-ref); new part → B23 | merged + new row |
| P1-14 | B4/B8 | merged (duplicate) |
| P1-15 | B9 | merged (duplicate) |
| P1-16 | B24 | new row |
| P1-17 | B25 | new row |
| S-1 | B26 | new row |
| S-2 | C6 | addendum |
| S-3 | C13 | new row |
| S-4 | C2 | merged (duplicate) |
| S-5 | C14 | new row |
| S-6 | E5 | merged (duplicate) |
| S-7 | I1–I4 | merged (duplicate) |
| S-8 | B27 (cross-ref B5) | new row |
| S-9 | C15 | new row |
| P3-1 | E7 | new row |
| P3-2 | G1 | merged (duplicate) |
| P3-3 | G8 | new row |
| P3-4 | E1 (addendum) + E8 | merged + new row |
| P3-5 | G3 | addendum |
| P3-6 | F1 | merged (duplicate) |
| P3-7 | C7 | merged (duplicate) |
| P3-8 | D3 | merged (duplicate) |
| P3-9 | F9 | new row |
| P3-10 | B28 | new row |
| P3-11 | C16 | new row |
| P3-12 | C12 | new row |
| P3-13 | C17 | new row |
| P3-14 | D4 | addendum |
| P3-15 | D1/D2 | merged (duplicate) |
| P3-16 | D5 | new row |
| P3-17 | K5 | new row (correction recorded in-row) |
| reviewer escalation | B14 | new row |

---

## Verification log

- 2026-09-25 — chunk 1 (`fix/audit-k1`): K1 **confirmed fixed on Linux GCC** by independent re-audit
  (all `Value.hpp` consumers compile clean under default `-Werror`). Same run unmasked **K2**
  (fixed in the same chunk, second commit). L1–L4 re-confirmed live-failing on Linux (curl empty
  reply, exit 139) — unchanged, awaiting chunk 2. Local serial `make` clean-tree EXIT=0;
  `test_runner` 249/249 with K1+K2 in. C10 idle-exit downgraded from "resolved (harness artifact)"
  to OPEN-unconfirmed per the independent run's scope note.
- 2026-09-25 — chunk 1 close-out, K3: fix = `$(BUILD)` rule added to the shared bootstrap
  (`Makefile`). Team's pre-fix repro (5/5 red) → post-fix local 5/5 green (`build` always created).
  Acceptance battery, **each run twice (serial / -j8)**:
  (1) `rm-rf-clean && make` EXIT 0 both ways, both binaries both ways, no `WARNINGS` override;
  (2) `test_runner` 249/249 after EACH build mode;
  (3) K1/K2 non-regression — six consecutive `-Werror` builds compiled `Value.hpp`+`SocketUtils.cpp`
  clean (any trip = build failure by policy);
  (4) N.4 triage after each build mode: steps 1–2 green; step 3 fails — on Windows the server exited
  pre-request in both triage runs (see C10 — now a confirmed startup-stability defect, distinct from
  the L-series), on Linux the POST death (L1–L4) stands per the team's runs. Step 4 unreachable in
  both. Chunk-2 findings L3a/L3b logged (inflight_ never written; L1 still open) — not applied.
- 2026-09-26 — Windows closure battery (post-merge `master@8a98356`, explicit per-platform loop):
  serial from-clean `make` EXIT=0; parallel from-clean `make -j8` EXIT=0 (12 green / 1 lost-output
  failure across all post-fix parallel attempts — one post-fix EXIT=2 occurred with output
  unfortunately discarded by the harness's own Out-Null; root `'No rule to make target'` failure
  mode eliminated, any residual flake unproven and still watched). `$(MKDIR)` resolution confirmed
  on MSYS2 by recipe echo: `powershell -NoProfile -Command New-Item ... -Path build/obj` — the
  K3 rule is idempotent there. `test_runner` 249/249 after each build mode (4x). POST-shape battery,
  3 attempts per tree state (pure master vs +chunk-2 WIP): NO crash observed in this session;
  shapes were EARLY-EXIT (C10) or ALIVE-AFTER-POST-with-timeout (curl 28, empty response — matches
  L3a's predicted silent-drop under WIP; the pure-master hang variant needs chunk-2 instrumentation
  to fully explain and is recorded, not masked). No observed shape is attributable to K1/K2/K3:
  K1's helpers are uninvoked at startup, K2's Windows code path is call-for-call identical, K3 is
  build-time only. Residual open items on Windows: C10 startup exit (~2/9 live runs here died
  pre-request; intermittent).
- 2026-10-08 — **2nd-pass audit merge** (branch `docs/audit-merge`; docs-only commit: this file +
  `tools/probes/btree_probe.cpp` + `tools/probes/commit_probe.cpp`). Merged the 2nd-pass P0/P1/S/P3
  findings per the reviewer's mapping (section X); added a Status column to every finding row
  (statuses per the reviewer's confirmed/UNVERIFIED lists); merged the two contradictory C10 rows
  into one (VERIFIED-LIVE); updated ground-truth HEAD to `69cb019`; recorded the K5 correction
  (stale-object hazard only, no K3 link), the OPEN DESIGN DECISION on the dispatcher, and the
  agreed-vs-proposed chunk order. B13/B14 probes committed and re-run at HEAD on Windows MSYS2
  GCC 14.2.0: btree_probe ascending 400/400 OK, descending 64/400, random 107/400 (point-gets
  400/400 in all orders; exit 1); commit_probe staged=400, commit-reachable=107, reader sees
  107/400 (exit 1) — consistent with the independent reviewer's Linux numbers (70/84; 78 reachable;
  see N.5). C10 re-confirmed live 2/2 (server exits pre-request, clean `bye`, exit 0). K3 hunt
  artifacts (`tmp_k3_*`) searched: not present in worktree. Chunk-2 WIP: confirmed in `stash@{0}`
  only; worktree clean at HEAD (see the L-series state note). No source, Makefile, or stash changes
  in this commit.

## Test-suite reality

"249/249 green" is real but never covered: named-column readback after SQL `VALUES` insert (B1),
UPDATE column-preservation (B2), txn control over the server (B3), body POST over the socket (L-series, N1),
multi-statement batches (B5), trailing-input rejection (B7), nullability/type rejection (B4/B8-enforcement —
this sentence previously referenced a nonexistent "B12"; the dangling reference is fixed and B12 is now the
JOIN/ORDER BY/LIMIT item), — and, decisively, **non-ascending BTree inserts**: `BTree_Put_Many_Rows_Triggers_Splits`
inserts ascending keys only, exactly the pattern that hides B13/B14 (see `tools/probes/`, N.5). A green suite
that structurally avoids every known failure shape certifies none of them (M2).
