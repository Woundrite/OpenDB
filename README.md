# AtomDB — A From-Scratch Modular SQL Database Engine

A C++23 database engine implementing the layered architecture described in
`.swarm/spec.md`: a **fixed non-pluggable core** (transaction management,
table-level locking, deadlock detection, command dispatch) with **pluggable
edges** (the front-end `ICommandSource` and the back-end `IStorageEngine`).

This build lands the **core types + core engine + in-memory storage + trivial
REPL + end-to-end smoke test** from the spec §7 roadmap milestones 1–5.

## Architecture

```
  ┌───────────────────  front ends (plugins)  ────────────────────┐
  │  SQL parser (planned)  REPL gateway (implemented)             │
  └───────────────────┬─────────────────────────────────────────────┘
                      ▼
                ICommandSource          ← universal pull contract
                      ▼
  ┌───────────────  CORE ENGINE ──────────────── fixed, not a plugin
  │  EngineLoop  TransactionManager  LockManager  DeadlockDetector  │
  └───────────────────────────────────────────────────┬────────────┘
                      ▼
                IStorageEngine            ← universal back-end contract
                      ▼
       InMemoryStorageEngine (v0.1)            ← versioned records
```

The core has zero dependencies on the edges; the edges can substitute freely.

## Building

The Makefile is **cross-platform** with auto-detection for `g++`, `clang++`,
and `cl` per spec §6.3. Strict warnings (`-Wall -Wextra -Wpedantic -Werror`)
and `-std=c++2b` (C++23).

```sh
make           # build/atomdb (REPL) + build/test_runner (unit tests)
make test      # 46 unit tests across types, core, storage, engine loop, REPL
make smoke     # INSERT -> SELECT round trip in the REPL (milestone 5)
make run       # launches the REPL with stdin/stdout attached
make clean     # rm -rf build/
```

### PowerShell / Windows

If `make` is unavailable or the bash/MinGW recipes mis-shell through `cmd.exe`,
use the equivalent PowerShell helper:

```powershell
./build.ps1 build    # build/atomdb
./build.ps1 test     # 46/46 tests
./build.ps1 smoke    # INSERT -> SELECT round trip
./build.ps1 clean
```

## Usage

After `make` (or `./build.ps1 build`):

```text
$ ./build/atomdb
atomdb v0.1 — type EXIT to quit
INSERT users {key:1,name:nikhil,age:30}
[OK]
SELECT users
[OK] | key | name   | age | _id |
    |-----+--------+-----+-----|
    | 1   | nikhil | 30  | 1   |
SELECT users WHERE name=nikhil
[OK] | key | name   | age | _id |
    | ...                              |
EXIT
bye
```

### Trivial REPL semantics

| Input                                              | Behavior                              |
| -------------------------------------------------- | -------------------------------------- |
| `INSERT <table> {<col>:<val>,...}`                 | Inserts a row (auto-id if absent)      |
| `SELECT <table>`                                   | Full table scan                         |
| `SELECT <table> WHERE <col>=<val>`                 | Predicate filter using `=`             |
| `EXIT`                                             | End-of-input                           |

Value parsing: integer literals → `Int64`; `"..."`/`'...'` → `Text`; `true`/`false` → `Bool`; `null` → `Null`.

### What's stubbed in v0.1 (per spec §5.4)

| Command         | Behavior                          |
| --------------- | --------------------------------- |
| UPDATE          | Returns `DbError::NotSupported`   |
| DELETE          | Returns `DbError::NotSupported`   |

Storage engine INTERNALS (put, remove, scan) already implement the
append-only versioned-records scheme — only the core engine surface for UPDATE
and DELETE is deferred to a future milestone.

## Test summary

46 tests across:

| Suite          | Count | Notes                                                   |
| -------------- | ----: | -------------------------------------------------------- |
| Value          |     6 | Tagged union, ordering across/within tags                |
| Tuple          |     6 | O(1) name lookup, order-sensitive equality              |
| Predicate      |     8 | AND/OR short-circuit, deep clone, missing-column false  |
| Core           |    16 | TX manager, lock matrix (8 cells), concurrent blocking, deadlock detection |
| Storage        |     6 | In-memory append-only versioned records, auto-id, tombstones |
| Engine loop    |     4 | End-to-end dispatch, UPDATE stubbed, REPL parsing       |
| **TOTAL**      | **46** | All pass under g++ 14.2.0 C++23 strict warnings         |

The deadlock detector test uses real concurrent blocking threads to construct
a genuine two-txn 2-cycle and verify the origin txn is reported as the victim
(spec §5.3).

## Spec decisions (resolved at build time)

| Open question (spec §9)        | Chosen resolution                                                            |
| ------------------------------ | ---------------------------------------------------------------------------- |
| INSERT key strategy (Q1)       | **Both** — `_id` column if provided, otherwise engine auto-assigns Int64     |
| Mutation semantics (Q2)        | **Append-only versioned records** — DELETE writes a tombstone                |
| Lock conflict behavior (Q3)    | **Block on wait queue** — `acquire()` waits on a condition_variable          |
| Scope of build                 | **Milestones 1–5** end-to-end smoke                                          |
