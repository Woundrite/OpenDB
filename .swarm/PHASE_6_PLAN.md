# Phase 6: SQL Surface Expansion + Storage Polish

Carries forward the deferred list from `PHASE_5_TECH_DEBT.md`. Scope is
intentionally narrow: low-to-medium items we can ship in-session without
adding subsystems (no OpenSSL, no RBAC, no query planner).

Each item has a concrete deliverable + tests. Exit criteria: all
green under `-Werror`, no regressions in the existing 190 tests.

## Items

### 6.1 ALTER TABLE — schema mutation API
**Goal:** `ALTER TABLE foo ADD COLUMN x INT;` / `DROP COLUMN x` / `RENAME TO bar`
**Surface change:** `IStorageProvider::alterTable(name, AlterSpec)` — opt-in
implementation in InMemory, LocalFile (metadata-page rewrite), Sharded (delegates
to children). Backwards compatible: existing tables persist across the change.
**Tests:** ~6 cases (add column on empty + populated, drop column, rename,
schema-change persists across reopen, sharded alter propagates).

### 6.2 SQL JOINs
**Goal:** `SELECT ... FROM a [INNER|LEFT] JOIN b ON a.x = b.y [WHERE ...]`
**Surface change:** SqlParser grammar extension + Command::fromTables +
nested-loop executor in EngineLoop. No optimizer; full table scan on each side.
**Tests:** ~6 cases (INNER basic, LEFT preserves unjoined LEFT rows, ON
with predicate, 3-table join, JOIN on InMemory + LocalFile, sharded join).

### 6.3 NULLS FIRST / NULLS LAST
**Goal:** `ORDER BY col [ASC|DESC] [NULLS FIRST|LAST]`
**Surface change:** OrderBySpec::nullsFirst bool; comparator extension in
EngineLoop in-memory sort.
**Tests:** ~4 cases (NULLS FIRST ASC, NULLS LAST DESC, default = LAST for
ASC, default = FIRST for DESC — PostgreSQL parity).

### 6.4 BTree per-page recycling
**Goal:** When BTree splits and a node is no longer referenced, return its
pages to the Pager free-list.
**Surface change:** Pager::freePage is called from BTree::removeNode +
BTree::mergeNodes. Tests prove freed pages appear in `Pager::freeListSize()`
after a remove-heavy workload.
**Tests:** ~4 cases (free after remove, free after merge, free survives
close/reopen, recycled pages are reused on subsequent inserts).

### 6.5 Best-fit allocator (replaces first-fit in Pager)
**Goal:** Best-fit reduces fragmentation when freeing mid-size pages.
**Surface change:** Pager's free-list becomes a `std::multimap<size_t, pageId>`
keyed on slab class (1 page, 2 pages, ..., 64+ pages). Allocate pops the
smallest slab >= requested; free coalesces adjacent pages if possible.
**Tests:** ~4 cases (best-fit picks smallest fitting, coalescing reduces
fragmentation, no regressions on existing 8 Pager tests).

### 6.6 HttpServer select-on-listen-fd (Windows accept busy-spin fix)
**Goal:** Accept loop currently busy-spins at 100% CPU on Windows because
`accept()` on a non-blocking listen fd returns WSAEWOULDBLOCK. Fix:
**Surface change:** io threads add the listen fd to their `select()` set so
accept() can block legitimately. AcceptLoop becomes one-shot or piggybacks
on the same io thread. Document the change in API.md.
**Tests:** ~3 cases (configurable accept-on-io-thread, no busy-spin on idle,
load test: 1000 concurrent connections don't crash).

## Out of scope (carried to a future Phase)

- Subqueries — needs query planner
- HTTPS via OpenSSL — vendor dependency
- Secondary indexes — needs storage + planner
- Authentication / RBAC — explicit out-of-scope per Phase 4 design decision

## Exit criteria

- [ ] 190 → 215+ tests, all green under `-Werror`
- [ ] ALTER TABLE / JOIN / NULLS shipped
- [ ] BTree recycles pages into Pager free-list
- [ ] Pager uses best-fit (or skip if regression risk too high)
- [ ] HttpServer accept-on-io-thread replaces busy-spin
- [ ] README + API.md updated to reflect Phase 6
- [ ] `plan.md`, `SWARM_PLAN.md`, `SWARM_PLAN.json` rewritten to reflect actual project state
