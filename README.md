# AtomDB — A From-Scratch Modular SQL Database Engine

A C++23 database engine implementing the layered architecture described in
`.swarm/spec.md`: a **fixed non-pluggable core** (transaction management,
locking, deadlock detection, command dispatch) with **pluggable edges**
(the front-end `ICommandSource` and the back-end `IStorageProvider`),
and a **pluggable page allocator** behind the storage Pager.

This build lands milestones 1–5 from the spec §7 roadmap plus most of
Phase 5 production-readiness work (HTTP server, row-level locks,
column-level DEFAULTs, sharded recovery, point-in-time backup,
operational metrics, SQL features, free-list reuse, concurrency stress),
and Phase 6.1 (ALTER TABLE), 6.2 (SQL JOINs), 6.4 (BTree per-page recycling),
6.5 (plugable buddy allocator).

## Architecture

```
  ┌───────────────────  front ends (plugins)  ────────────────────┐
  │  HttpApi  SqlParser  ReplSource                                │
  │  (REST)   (SQL)      (toy REPL)                                │
  └───────────────────┬─────────────────────────────────────────────┘
                       ▼
                 ICommandSource          ← universal pull contract
                       ▼
  ┌───────────────  CORE ENGINE ──────────────── fixed, not a plugin
  │  EngineLoop  EngineDispatcher (thread pool)                     │
  │  TransactionManager  LockManager  DeadlockDetector              │
  └───────────────────────────────────────────────────┬────────────┘
                       ▼
                 IStorageEngine / IStorageProvider  ← universal back-end
                       ▼
        LocalFileStorageEngine  InMemoryStorageEngine  CachingStorageEngine
        ShardedStorageProvider  (composition over inheritance)
```

The core has zero dependencies on the edges; the edges can substitute freely.

## Building

The build script is **cross-platform** with auto-detection for `g++`,
`clang++`, and `cl` per spec §6.3. Strict warnings
(`-Wall -Wextra -Wpedantic -Werror`) and `-std=c++2b` (C++23).

```sh
make           # build/atomdb (REPL) + build/test_runner (unit tests)
make test      # 209 unit tests across types, core, storage, engine loop, REPL, joins, ...
make smoke     # INSERT -> SELECT round trip in the REPL (milestone 5)
make run       # launches the REPL with stdin/stdout attached
make clean     # rm -rf build/
```

### PowerShell / Windows

If `make` is unavailable, use the equivalent PowerShell helper:

```powershell
./build.ps1 build    # build/atomdb
./build.ps1 test     # 209/209 tests
./build.ps1 smoke    # INSERT -> SELECT round trip
./build.ps1 clean
```

## Usage

### SQL via the HttpApi

```sh
./build/atomdb --http-port 8080 &
curl -X POST http://localhost:8080/query \
  -H 'Content-Type: application/json' \
  -d '{"type":"query","sql":"CREATE TABLE users(id INT PRIMARY KEY, name TEXT, age INT)"}'
curl -X POST http://localhost:8080/query \
  -H 'Content-Type: application/json' \
  -d '{"type":"query","sql":"INSERT INTO users VALUES (1, '\''nikhil'\'', 30)"}'
curl -X POST http://localhost:8080/query \
  -H 'Content-Type: application/json' \
  -d '{"type":"query","sql":"SELECT * FROM users WHERE age > 25 ORDER BY name ASC LIMIT 10"}'
curl -X POST http://localhost:8080/query \
  -H 'Content-Type: application/json' \
  -d '{"type":"query","sql":"SELECT u.name, o.amt FROM users u INNER JOIN orders o ON u.id = o.uid WHERE o.amt > 50 ORDER BY u.name ASC"}'
```

JOINs use fully-qualified column names in projections, ON, WHERE, and ORDER BY
(e.g. `u.name`, `o.uid`). LEFT JOIN preserves unjoined left rows with right-side
columns as NULL. Multi-table chains are supported (`a JOIN b ON ... JOIN c ON ...`).

### Trivial REPL

After `make`:

```text
$ ./build/atomdb
atomdb v0.1 — type EXIT to quit
INSERT users {key:1,name:nikhil,age:30}
[OK]
SELECT users
[OK] | key | name   | age | _id |
    |-----+--------+-----+-----|
    | 1   | nikhil | 30  | 1   |
EXIT
bye
```

Value parsing: integer literals → `Int64`; `"..."`/`'...'` → `Text`; `true`/`false` → `Bool`; `null` → `Null`.

## Test summary

216 tests across:

| Suite             | Count | Notes                                                   |
| ----------------- | ----: | -------------------------------------------------------- |
| Value             |    13 | Tagged union, ordering across/within tags                |
| Tuple             |     6 | O(1) name lookup, order-sensitive equality              |
| Predicate         |     8 | AND/OR short-circuit, deep clone, missing-column false  |
| Core              |    24 | TX manager, lock matrix, row-level locks, deadlock       |
| Storage           |     8 | Append-only versioned records, MVCC visible_seq cutoff   |
| Pager             |    15 | Free-list push/pop, LIFO order, survives reopen, buddy  |
| BTree             |    14 | Put/remove, split, ordered scan, MVCC                   |
| EngineLoop        |     8 | End-to-end dispatch, ORDER BY/LIMIT/OFFSET              |
| EngineDispatcher  |     4 | Metrics snapshot shape, shutdown, 8-session concurrent   |
| LocalFile         |    18 | Persists across reopen, Backup snapshots                |
| Sharded           |    16 | Hash/Range/List partitioning, recovery                   |
| Caching           |     3 | Cache hit/miss                                          |
| HttpApi           |    10 | JSON encoding, DEFAULT materialization, OpenAPI-ish     |
| HttpServer        |     6 | Multi-threaded async server lifecycle                    |
| JsonEncoder       |     7 | Base64/ISO-8601/array-of-arrays                          |
| SqlParser         |    34 | DDL/DML/TXN, DEFAULT, WHERE/ORDER BY/LIMIT/OFFSET        |
| Stress            |     3 | 8-thread INSERT, 4-thread REMOVE, 6-thread reads         |
| **TOTAL**         | **216** | All pass under g++ 14.2.0 C++23 strict warnings        |

The deadlock detector test uses real concurrent blocking threads to construct
a genuine two-txn 2-cycle and verify the origin txn is reported as the victim
(spec §5.3).

## Spec decisions (resolved at build time)

| Open question (spec §9)        | Chosen resolution                                                            |
| ------------------------------ | ---------------------------------------------------------------------------- |
| INSERT key strategy (Q1)       | **Both** — `_id` column if provided, otherwise engine auto-assigns Int64     |
| Mutation semantics (Q2)        | **Append-only versioned records** — DELETE writes a tombstone                |
| Lock conflict behavior (Q3)    | **Block on wait queue** — `acquire()` waits on a condition_variable          |
| Storage provider vs engine     | **Composition**: provider owns engine (per spec §4.3)                         |
| WAL / CrashDurable             | **Phase 5**: v0.1 has `commit() → saveMetadata() → pager.sync()` barrier     |
| Concurrency                    | **Multi-threaded**: EngineDispatcher with worker pool; row-level locks        |
| HTTP server                    | **Async + multi-threaded**: 1 accept thread, N io threads with select loop    |
