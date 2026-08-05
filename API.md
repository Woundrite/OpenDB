# AtomDB API Reference

This document describes the public contracts (Phase 4+) for extending or
embedding AtomDB. Items are grouped by layer (back-end contract, core types,
front-end contract) and listed by header.

## Back-end contracts

### `atomdb::IStorageProvider` (`contracts/IStorageProvider.hpp`)

The back-end plugin contract that owns the data path, capability
declaration, lifecycle, and schema/DDL surface.

```cpp
class IStorageProvider {
public:
    virtual ~IStorageProvider() = default;

    // Identity / capability probing
    virtual std::string  name() const = 0;
    virtual std::uint32_t capabilities() const = 0;  // bitmask of StorageCapability
    virtual TypeVocabulary typeVocabulary() const = 0;

    // Lifecycle
    virtual DbError open(const std::string& uri) = 0;
    virtual DbError close() = 0;
    virtual bool    isOpen() const noexcept = 0;

    // Schema (DDL surface; owned by the provider, not the core)
    virtual DbError createTable(const Schema& schema) = 0;
    virtual DbError dropTable(const std::string& name) = 0;
    virtual std::optional<Schema> describeTable(const std::string& name) const = 0;
    virtual std::vector<std::string> tables() const = 0;

    // Engine access
    virtual IStorageEngine* engine() = 0;
};
```

**Capability flags** (`StorageCapability`):
- `Durable` — data survives process exit
- `CrashDurable` — data survives mid-write crash
- `RandomAccess` — O(log n) `get()` by key
- `OrderedScan` — `scan()` emits rows in key order
- `BlobSupport` — accepts `Value::Blob`
- `TemporalSupport` — accepts `Value::Date`/`Value::Timestamp`
- `Concurrent` — safe for concurrent worker threads
- `Networked` — backing store is remote

**Composition model**: a provider IMPLEMENTS `IStorageProvider` and OWNS an
`IStorageEngine*` (returned via `engine()`). Decorator providers (Caching,
Sharded) hold another `IStorageProvider*` and forward / merge.

### `atomdb::IStorageEngine` (`contracts/IStorageEngine.hpp`)

The contract above the engine layer. Every concrete engine (In-Memory,
Local B-Tree, Caching decorator, Sharded) implements this interface.
The core engine only ever holds an `IStorageEngine*`.

```cpp
class IStorageEngine {
public:
    virtual ~IStorageEngine() = default;

    // DML (txn-scoped)
    virtual std::optional<Tuple> get(TxnId txn,
                                       const std::string& table,
                                       const Value& key) = 0;
    virtual DbError put(TxnId txn,
                        const std::string& table,
                        const Value& key,
                        const Tuple& row) = 0;
    virtual DbError remove(TxnId txn,
                            const std::string& table,
                            const Value& key) = 0;
    virtual void scan(TxnId txn,
                      const std::string& table,
                      const std::function<void(const Tuple&)>& emit) = 0;

    // Two-phase commit lifecycle
    virtual DbError prepare(TxnId txn) = 0;
    virtual DbError commit(TxnId txn) = 0;
    virtual DbError commit(TxnId txn, std::uint64_t visibleSeq) = 0;
    virtual DbError abort(TxnId txn) = 0;
};
```

Two-phase commit lifecycle (spec §4.2):
- `prepare(txn)` — engine may flush to disk here if durable
- `commit(txn)` or `commit(txn, visibleSeq)` — make staged writes visible
- `abort(txn)` — discard all staged writes for this txn

## Front-end contracts

### `atomdb::ICommandSource` (`contracts/ICommandSource.hpp`)

The pull-based contract the core uses to obtain commands.

```cpp
class ICommandSource {
public:
    virtual ~ICommandSource() = default;

    // Returns std::nullopt on EOF. The core loops calling nextCommand()
    // until nullopt.
    virtual std::optional<Command> nextCommand() = 0;

    // Sink for results. Two overloads — one for success, one for error.
    virtual void present(const ResultSet& rs) = 0;
    virtual void present(const DbError& err) = 0;
};
```

### `atomdb::IAccessPlugin` (`contracts/IAccessPlugin.hpp`)

The server-mode access plugin (REPL-style front-end, HTTP API, etc.).
Provides an open/close lifecycle and a `handleRequest(req)` method.

```cpp
class IAccessPlugin {
public:
    virtual ~IAccessPlugin() = default;

    virtual DbError open(const std::string& uri,
                         IStorageProvider* storage,
                         EngineDispatcher* dispatcher) = 0;
    virtual DbError close() = 0;
    virtual std::string handleRequest(const std::string& req) = 0;
};
```

## Core types

### `atomdb::Value` (`types/Value.hpp`)

Tagged-union value type with strong ordering. Tags:
`Null`, `Bool`, `Int32`, `Int64`, `Double`, `Text`, `Blob`, `Date`,
`Timestamp`.

Ordering buckets: `Null(0) < Bool(1) < Number(2) < Text(3) < Blob(4)`.

### `atomdb::Tuple` (`types/Tuple.hpp`)

Ordered sequence of `ColumnValue{name, Value}` pairs. O(1) name lookup,
order-sensitive equality.

### `atomdb::Schema` (`types/Schema.hpp`)

A table's column definitions + optional `PartitionPolicy`. Includes
`ColumnDef::defaultValue : std::optional<Value>` for column-level
DEFAULTs.

### `atomdb::Command` (`types/Command.hpp`)

The IR for DML operations: `Insert`, `Update`, `Delete`, `Select`. Has
optional `where` predicate, projections, ORDER BY, LIMIT, OFFSET, and a
`joins` vector of `JoinClause` records (each carrying `kind` Inner/Left,
right `table`, and the single-equality ON predicate as two fully-qualified
columns). Joined tuples carry `<table>.<column>` keys so columns from
different tables never collide.

### `atomdb::Predicate` (`types/Predicate.hpp`)

Tree of `ComparisonNode` / `LogicalNode` for WHERE evaluation.

## Core engine

### `atomdb::EngineLoop` (`core/EngineLoop.hpp`)

The synchronous core dispatch loop. Pulls commands from the
`ICommandSource`, acquires row- or table-level locks, dispatches by
command type, and commits at the end of each iteration.

### `atomdb::EngineDispatcher` (`core/EngineDispatcher.hpp`)

A thread-pool wrapper. Hands Commands to a worker thread that runs the
`EngineLoop` against the storage engine.

### `atomdb::TransactionManager` (`core/TransactionManager.hpp`)

Owns the `visibleSeq` counter, tracks in-flight TXs, and produces
TxnIds. `commitTxn` advances `visibleSeq`.

### `atomdb::LockManager` (`core/LockManager.hpp`)

Shared/Exclusive locks with blocking semantics. Supports both
table-level and row-level granularity via `ResourceKey = (table, rowKey)`.

```cpp
class LockManager {
public:
    void acquire(TxnId, const std::string& table, LockMode);  // table-level
    void acquireKey(TxnId, const ResourceKey&, LockMode);     // row-level
    void release(TxnId);
    void releaseKey(TxnId, const ResourceKey&);
    bool isGranted(TxnId, const std::string& table) const;
    bool isGrantedKey(TxnId, const ResourceKey&) const;
    std::vector<TxnId> getHolders(const std::string& table) const;
    std::vector<TxnId> getWaiters(const std::string& table) const;
};
```

### `atomdb::DeadlockDetector` (`core/DeadlockDetector.hpp`)

Single wait-chain traversal. Returns the victim TxnId if a cycle is
reachable from `origin`, else `std::nullopt`.

## Front-ends

### `atomdb::HttpServer` (`frontend/HttpServer.hpp`)

Multi-threaded HTTP/1.1 server bound to a TCP socket. 1 accept thread,
N io threads. Each io thread runs a `select()` loop with 100ms timeout.

Endpoints:
- `POST /query`     — SQL via `HttpApiAccessPlugin`
- `POST /begin`     — open transaction
- `POST /commit`    — commit (body `txnId`)
- `POST /rollback`  — abort (body `txnId`)
- `GET  /health`    — liveness
- `GET  /status`    — snapshot of active connections + request count
- `GET  /metrics`   — full `Stats` JSON

### `atomdb::HttpApiAccessPlugin` (`frontend/HttpApi.hpp`)

Stateless JSON request handler. JSON format documented in the file's
header comment. Recognized request types: `query`, `begin`, `commit`,
`rollback`, `describe`.

### `atomdb::SqlParser` (`frontend/SqlParser.hpp`)

Hand-rolled recursive-descent parser for a small SQL subset. Supported:
- `CREATE TABLE foo (col type [NOT NULL] [PRIMARY KEY] [DEFAULT <val>], ...)`
- `CREATE TABLE foo (...) PARTITION BY HASH(col) PARTITIONS N`
- `DROP TABLE foo`
- `INSERT INTO foo [cols] VALUES (val,...)` or `DEFAULT VALUES`
- `SELECT [* | tbl.col,...] FROM foo [INNER|LEFT] JOIN bar ON foo.x = bar.y [WHERE <pred>] [ORDER BY col [ASC|DESC] [NULLS FIRST|LAST],...] [LIMIT n] [OFFSET n]`
- `UPDATE foo SET col=val,... [WHERE <pred>]`
- `DELETE FROM foo [WHERE <pred>]`
- `BEGIN [TRANSACTION]` / `COMMIT` / `ROLLBACK`

### `atomdb::ReplSource` (`frontend/ReplSource.hpp`)

Tiny line-based REPL front-end. Toy dialect:
`INSERT <table> {<col>:<val>,...}`, `SELECT <table> [WHERE <col>=<val>]`,
`EXIT`.

## Storage backends

### `atomdb::LocalFileStorageProvider` (`storage/LocalFileStorageProvider.hpp`)

Durable file-backed provider. MVCC B+Tree per table on a 4 KiB-paged
file with CRC32 tear detection. `commit() → saveMetadata() → pager.sync()`
is the durability barrier. Includes `backupTo(target_uri)` for
point-in-time snapshots (Phase 5 Item 16).

### `atomdb::InMemoryStorageProvider` (`storage/InMemoryStorageProvider.hpp`)

RAM-only provider. Append-only versioned records keyed by user_key +
commitSeq DESC. Useful for tests.

### `atomdb::ShardedStorageProvider` (`storage/ShardedStorageProvider.hpp`)

Routes DML by hash (default), Range, or List partition policy. Owns N
child `IStorageProvider*`. Schema is propagated to all shards.

### `atomdb::CachingStorageEngine` (`storage/CachingStorageEngine.hpp`)

Read-through + write-through cache decorator. Wraps another
`IStorageEngine*`.

### `atomdb::IPageAllocator` (`contracts/IPageAllocator.hpp`)

Pluggable page allocation strategy behind the Pager. The Pager owns an
`std::unique_ptr<IPageAllocator>` (defaults to `BuddyPageAllocator`).

```cpp
class IPageAllocator {
public:
    using PageId = std::uint32_t;
    static constexpr PageId INVALID_PAGE = 0xFFFFFFFFu;

    // Allocate `n` contiguous pages (n >= 1). Returns first PageId of run.
    virtual PageId allocatePages(std::size_t n,
        std::function<PageId(std::size_t)> extendFile) = 0;

    // Free a contiguous run of `n` pages starting at `start`.
    virtual void freePages(PageId start, std::size_t n) = 0;

    // Total free pages (test diagnostic).
    virtual std::size_t freePageCount() const = 0;

    // Number of distinct free runs (test diagnostic).
    virtual std::size_t freeRunCount() const = 0;

    // File was extended by `n` pages at `startPageId`.
    virtual void onFileExtended(PageId startPageId, std::size_t n) = 0;

    // Serialize 16-byte allocator header (written to page 0 offset 12).
    virtual void serializeHeader(std::uint8_t out[16]) const = 0;

    // Load 16-byte allocator header from page 0 offset 12.
    virtual void loadHeader(const std::uint8_t in[16], PageId pageCount) = 0;

    virtual ~IPageAllocator() = default;
};
```

**Built-in implementation** (`storage/BuddyPageAllocator.hpp`):

- **Binary buddy allocator** with slab classes 1, 2, 4, 8, 16, 32, 64, 128, 256 pages.
- O(1) allocation/free under a single mutex.
- Internal fragmentation ≤ 50% (round-up to power of two); coalescing on free
  recovers adjacent runs.
- On-disk format v2: page 0 offset 12 holds 16-byte diagnostic header
  (`maxClass`, `runCount`); the v1 singly-linked free-list chain is preserved
  in freed pages for backward compatibility. Migration from v1 files is
  automatic on open.

**Usage**:
```cpp
auto pager = std::make_unique<Pager>(path);           // uses BuddyPageAllocator
auto pager = std::make_unique<Pager>(path,
    std::make_unique<SlabClassPageAllocator>());      // future swap
```

Tests: `Pager_Buddy_AllocatePages_One_Returns_First_Free`,
`Pager_Buddy_AllocatePages_Two_Requests_2p_From_2p_Slab`,
`Pager_Buddy_AllocatePages_Three_Rounds_Up_To_4p_Slab`,
`Pager_Buddy_Free_Coalesces_Two_Adjacent_2p_Into_4p`,
`Pager_Buddy_Free_Recursively_Coalesces_To_Max_Class`,
`Pager_Buddy_V1_To_V2_Migration_Preserves_Allocations_And_Frees`,
`BTree_Large_Row_Stored_Across_MultiPage_Run_And_Freed_On_Delete`.

## JSON encoding

### `atomdb::JsonEncoder` (`frontend/JsonEncoder.hpp`)

Encodes `ResultSet` and `DbError` to compact JSON. Special handling for
`Blob` (base64), `Date`/`Timestamp` (ISO-8601), and array-of-arrays row
representation.
