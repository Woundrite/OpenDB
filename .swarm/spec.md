# AtomDB — Technical Specification

**Version:** 0.1 (Core Engine Design Phase)
**Status:** Draft — core loop designed, storage engine not yet implemented
**Owner:** Nikhil

---

## 1. Overview

AtomDB is a modular, layered SQL database engine written in C++23. The architecture
separates a **fixed core** (transaction management, locking, deadlock detection, query
execution) from **pluggable edges**: front-end command sources (SQL parser, REPL, API
gateway) and back-end storage engines (local B-Tree, sharded, cached, remote).

The design goal is extensibility without rearchitecture: new front-ends and back-ends
should be addable as independent modules that satisfy a fixed interface, without
touching the core engine's transaction, locking, or execution logic.

### 1.1 Design Principles

1. **The core never depends on the edges.** The core engine knows about two interface
   boundaries — a command source and a storage engine — and nothing else about how input
   arrives or how data is physically stored.
2. **Interfaces are contracts, not conveniences.** Every interface boundary in this spec
   is intended to be stable; new features extend the interface only when the abstraction
   genuinely can't express the feature otherwise.
3. **Composition over inheritance for extension.** Storage engines can wrap other storage
   engines (e.g., a caching layer in front of a sharded backend) rather than needing deep
   class hierarchies.
4. **Correctness before performance.** The first working version favors simple, obviously
   correct implementations (table-level locking, single-chain deadlock detection,
   in-memory storage) over optimized ones. Optimization is a distinct, later phase.
5. **No silent scope creep in the core.** Transactions, locks, and deadlock detection are
   coupled to each other by nature and live in the core engine, not as plugins — this was
   an explicit decision after considering (and rejecting) a generic event-bus architecture
   for this part of the system.

### 1.2 Non-Goals (for this version)

- Distributed transactions (2PC/Raft/Paxos) — deferred until single-node correctness is
  proven.
- Row-level locking — starting at table-level granularity; row-level is a planned
  upgrade, not a v0.1 requirement.
- Query optimization beyond predicate evaluation — no cost-based optimizer yet.
- Persistence guarantees (WAL, crash recovery) — the first storage engine is in-memory
  only; durability is a separate milestone.

---

## 2. System Architecture

### 2.1 Layer Diagram

```
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ SQL Parser   │  │ REPL Gateway │  │ HTTP/gRPC API│   ← front-end plugins
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │                 │                 │
       └────────────┬────┴─────────────────┘
                     ▼
         Command Source Contract → Command (universal IR)
                     ▼
       ┌─────────────────────────────────┐
       │            CORE ENGINE           │   ← fixed, not pluggable
       │  Engine Loop                     │
       │  Transaction Manager             │
       │  Lock Manager                    │
       │  Deadlock Detector               │
       └─────────────────┬───────────────┘
                          ▼
              Storage Engine Contract (universal)
                          ▼
       ┌──────────┬───────────────┬──────────────┐
       │ In-Memory │ Local B-Tree  │ Sharded /    │   ← back-end plugins
       │ (v0.1)    │ (planned)     │ Cached/Remote│
       └──────────┴───────────────┴──────────────┘
```

The core engine is the only box in this diagram that is not swappable. Everything above
it and everything below it can be replaced independently, as long as the replacement
satisfies the relevant contract.

### 2.2 Command Execution Pipeline

This is the flow a single command takes from arrival to result.

```
                        ┌─────────────────────┐
                        │  Front-end produces  │
                        │  a Command object    │
                        └──────────┬───────────┘
                                   ▼
                     ┌─────────────────────────────┐
                     │ Does Command carry an open   │
                     │ TxnId already?               │
                     └──────────┬───────────┬───────┘
                          No     │           │  Yes
                                 ▼           ▼
                   ┌───────────────────┐  ┌─────────────────────┐
                   │ Begin new          │  │ Reuse existing       │
                   │ transaction         │  │ transaction id       │
                   │ (auto-commit mode)  │  │ (caller manages       │
                   │                     │  │  commit/rollback)     │
                   └──────────┬─────────┘  └──────────┬───────────┘
                              └───────────┬────────────┘
                                          ▼
                       ┌───────────────────────────────────┐
                       │ Determine required lock mode:       │
                       │  SELECT → Shared                    │
                       │  INSERT/UPDATE/DELETE → Exclusive    │
                       └──────────────────┬───────────────────┘
                                          ▼
                       ┌───────────────────────────────────┐
                       │ Attempt to acquire table lock       │
                       └──────────────────┬───────────────────┘
                             Granted        │        Denied
                                 │          ▼          │
                                 │  ┌────────────────────────┐
                                 │  │ Record this txn as       │
                                 │  │ waiting on the table      │
                                 │  └───────────┬────────────┘
                                 │              ▼
                                 │  ┌────────────────────────┐
                                 │  │ Walk wait-for chain from  │
                                 │  │ this txn — cycle found?   │
                                 │  └─────┬──────────────┬─────┘
                                 │    Yes  │              │  No
                                 │         ▼              ▼
                                 │  ┌─────────────┐  ┌───────────────┐
                                 │  │ Abort txn,   │  │ Return "lock   │
                                 │  │ release      │  │ busy, retry"   │
                                 │  │ locks, send   │  │ error to caller │
                                 │  │ deadlock error │  └───────────────┘
                                 │  └─────────────┘
                                 ▼
                    ┌─────────────────────────────────┐
                    │ Dispatch to handler based on type: │
                    │  Select / Insert / Update / Delete │
                    └──────────────────┬─────────────────┘
                                       ▼
                    ┌─────────────────────────────────┐
                    │ Handler calls into Storage Engine  │
                    │ contract (get/put/remove/scan)     │
                    └──────────────────┬─────────────────┘
                                       ▼
                    ┌─────────────────────────────────┐
                    │ Was this an auto-commit command?    │
                    └───────────┬───────────┬─────────────┘
                            Yes  │           │  No
                                 ▼           ▼
              ┌────────────────────────┐  ┌───────────────────────┐
              │ Prepare → Commit storage │  │ Leave txn open;         │
              │ + txn manager; release    │  │ locks remain held for   │
              │ locks                     │  │ caller's next command    │
              └────────────┬─────────────┘  └───────────────────────┘
                           └───────────────┬────────────────────────┘
                                           ▼
                            ┌───────────────────────────┐
                            │ Send ResultSet or DbError    │
                            │ back through the front-end    │
                            └───────────────────────────┘
```

---

## 3. Core Data Model

### 3.1 Type Dependency Graph

This governs include order and, by extension, build order.

```
        Value
     (no dependencies —
      tagged union of
      Int32/Int64/Double/
      Text/Bool/Null)
          │
   ┌──────┼───────────┐
   ▼      ▼           ▼
ColumnValue  Tuple   Predicate
(name+value   (ordered,    (recursive
 pair)        named row    expression
              of Values,  tree: leaf =
              O(1) lookup Comparison,
              by column  internal =
              name)       Logical And/Or)
                │              │
                └──────┬───────┘
                       ▼
                    Command
        (universal IR: type, table,
         optional Predicate tree,
         Tuple of values, projection
         column list, TxnId)
```

### 3.2 Entity Descriptions

| Type | Role | Key Property |
|---|---|---|
| Transaction Id | Strongly-typed transaction handle | Distinct type from raw integers to prevent accidental mixing with page IDs, table IDs, etc. |
| Value | Tagged union scalar (Int32, Int64, Double, Text, Bool, Null) | The atomic unit of storage — every column, every predicate operand is a Value |
| Column Value | A single (column name, Value) pair | Used where ordered iteration matters more than lookup |
| Tuple | A full row — ordered, named Values with fast lookup | Represents both storage records and query results |
| Predicate | Recursive WHERE-clause expression tree | Leaf nodes compare a column to a value; internal nodes combine children with AND/OR |
| Command | Universal intermediate representation | The only object that crosses the front-end → core boundary; no front-end may pass raw SQL/JSON past this point |
| Result Set | Rows returned from a query | Carries a success flag alongside row data |
| Db Error | Structured error with a message | Includes named constructors for common cases (deadlock, not-found) |

### 3.3 Predicate Tree — Worked Example

Example: `WHERE age > 25 AND name = 'nikhil'`

```
              Logical Predicate
                  (AND)
                 /      \
     Comparison            Comparison
     column: age            column: name
     op: Greater-than        op: Equals
     operand: 25             operand: 'nikhil'
```

Evaluation walks this tree against a candidate row: an AND node short-circuits to false
as soon as any child evaluates false; an OR node short-circuits to true as soon as any
child evaluates true; a comparison leaf looks up its column in the row and applies the
operator directly.

---

## 4. Plugin Contracts

### 4.1 Command Source Contract (Front-End Boundary)

**Responsibilities of any implementation:**
- Produce the next Command, or signal that input has ended.
- Accept a Result Set and present it back to whatever the front-end represents (a
  terminal, an HTTP response, a socket).
- Accept a Db Error and present it similarly.

**Implementations, planned status:**

| Implementation | Status | Role |
|---|---|---|
| SQL Parser Source | Planned | Parses a SQL subset from a stream into Command objects |
| REPL Gateway Source | Planned | Interactive line-based front-end, formatted table output |
| HTTP/gRPC API Source | Future | Translates JSON/protobuf requests into a Command |

### 4.2 Storage Engine Contract (Back-End Boundary)

**Responsibilities of any implementation:**
- Point lookup, insert, and delete of a record by key, scoped to a transaction.
- Provide a scan cursor over an entire table.
- Participate in a two-phase commit-style lifecycle: prepare, commit, abort — even a
  purely local engine implements prepare as a trivial success.

**Implementations, planned status:**

| Implementation | Status | Role |
|---|---|---|
| In-Memory Engine | **Next milestone** | Map-backed, no persistence — needed to exercise the full core loop end-to-end |
| Local B-Tree Engine | Planned | Real paged disk storage with a B-Tree index and write-ahead log |
| Caching Engine | Planned | Decorator that wraps another storage engine with a read cache |
| Sharded Engine | Future | Routes by key hash/range across multiple storage engine instances |

### 4.3 Back-End Composition Pattern

```
   Core Engine
       │
       ▼
  Caching Engine  ──── wraps ────▶  Sharded Engine
                                          │
                          ┌───────────────┼───────────────┐
                          ▼               ▼               ▼
                     Shard A          Shard B          Shard C
                  (Local B-Tree)   (Local B-Tree)   (Local B-Tree)
```

The core only ever holds a reference to the outermost layer (Caching Engine in this
example) and is unaware of everything beneath it.

---

## 5. Core Engine Components

### 5.2 Lock Compatibility Matrix

| Held by others \ Requested | Shared | Exclusive |
|---|---|---|
| None held | Grant | Grant |
| Shared held | Grant | Deny |
| Exclusive held | Deny | Deny |

Self-held locks (the requesting transaction already holds a lock on the same table) are
always compatible with a new request from that same transaction, including upgrade from
Shared to Exclusive.

**Granularity note:** locking is table-level in v0.1, not row-level.

### 5.3 Deadlock Detection Flow

Single wait-chain traversal from the requesting txn; cycle that loops back to the
origin implies deadlock, the origin transaction is aborted.

**Known limitation:** this walks a single chain per detection call. A complete
multi-transaction cycle detector should explore all outgoing wait edges per transaction
(a full graph traversal), not just the one currently being followed.

---

## 9. Open Questions

1. INSERT key strategy — primary keys / auto-increment, or explicit key required?
2. UPDATE/DELETE mutation semantics — in-place overwrite vs. append-only versioned records?
3. Blocking vs. failing on lock conflict — current behavior fails immediately.
4. Target tier — resume-worthy vs. production-grade vs. HFT-adjacent — changes priority order.
