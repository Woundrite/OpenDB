#ifndef OPENDB_SCHEMA_HPP
#define OPENDB_SCHEMA_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "opendb/types/Value.hpp"

// Schema (spec §3.2, Phase 4 extension): describes a table's column types,
// primary key, and (optionally) partition policy. The schema is owned by the
// storage provider, not the core engine. Front-ends (SQL parser, HTTP API)
// query the provider's describeTable() to learn the column types before
// issuing INSERTs or producing wire-format payloads.
//
// Type vocabulary is provider-declared: a ColumnDef whose declared `type` is
// not in the connected provider's TypeVocabulary.accepted list MUST be rejected
// by IStorageProvider::createTable() with DbError::notSupported. The core never
// asserts "this type is supported" — it asks the provider.
//
// Partition policy (Phase 4): PostgreSQL-style declarative partitioning.
// `PARTITION BY HASH(col) PARTITIONS N` in DDL becomes a PartitionPolicy with
// kind=Hash, column="col", shardCount=N. The ShardedStorageProvider parses the
// policy at createTable() and routes put/get/remove by hashing the partition
// key column's value. The child shards are themselves concrete IStorageProviders
// (LocalFileStorageProvider instances in the common case, InMemoryStorageProvider
// in tests).

namespace opendb {

// A single column's declaration. Provider-enforced constraints (maxByteLength,
// nullable) are checked inside the provider's put(); the core does not validate.
struct ColumnDef {
    std::string name;
    ValueType   type = ValueType::Null;

    // If false, the column MUST NOT be null in inserted rows. The provider's
    // put() rejects null values for non-nullable columns with DbError::notSupported
    // or (if available) a more specific TypeMismatch error.
    bool nullable = true;

    // If true, this column is (part of) the primary key. Per Phase 4 design,
    // at most ONE column may be the primary key; composite PKs are a future
    // enhancement that would replace this bool with a `std::vector<std::string>`
    // at the Schema level.
    bool primaryKey = false;

    // Provider-enforced max byte length for Text and Blob columns. 0 = unlimited.
    // The provider MAY silently truncate longer values or MAY reject with
    // DbError::notSupported; the contract leaves this to the provider.
    std::uint64_t maxByteLength = 0;

    // Provider-specific free-form metadata. Examples:
    //   {"seed":"42"}               — auto-increment seed (LocalFile provider)
    //   {"collation":"en_US.UTF-8"} — text collation (future)
    //   {"default":"'1970-01-01'"}  — server-side default (future)
    std::unordered_map<std::string, std::string> extensions;

    // Phase 5 Item 7: column-level DEFAULT value. Used by INSERT (when the
    // column is omitted) and by INSERT ... DEFAULT VALUES. Stored as a
    // Value (not as a string in extensions) so that the parser produces a
    // typed Value directly — INT default 42 -> Value::int64(42).
    // Optional: std::nullopt means "no default declared".
    std::optional<Value> defaultValue;
};

// Declarative partition policy (PostgreSQL-style). Owned by the schema and
// interpreted by the ShardedStorageProvider. `None` means the table is not
// partitioned and lives in a single shard.
struct PartitionPolicy {
    enum class Kind : std::uint8_t {
        None,
        Hash,   // hash(partitionKey) % shardCount
        Range,  // boundaries define explicit shard edges
        List,   // lists define explicit shard membership
    };

    Kind         kind         = Kind::None;
    std::string  column;                   // partition key column name
    std::uint32_t shardCount  = 0;          // for Hash
    // For Range: boundaries[i] = lower bound of shard i. Row goes to shard i
    //   iff row.column >= boundaries[i-1] && row.column < boundaries[i].
    //   boundaries MUST be sorted ascending and have shardCount+1 entries
    //   (with -inf and +inf implied by absence at either end).
    std::vector<Value> boundaries;
    // For List: lists[i] = values routed to shard i. Row goes to shard i
    //   iff row.column ∈ lists[i]. Each list element must equal the column's
    //   Value exactly (tag-true equality per Value::operator==).
    std::vector<std::vector<Value>> lists;

    bool isPartitioned() const noexcept { return kind != Kind::None; }
};

// A table's full schema. Queryable through IStorageProvider::describeTable().
struct Schema {
    std::string                       table;
    std::vector<ColumnDef>            columns;
    std::optional<PartitionPolicy>     partition;

    // Lookups used by the SQL parser, API encoder, UPDATE/DELETE handlers.
    const ColumnDef* find(const std::string& col) const {
        for (const auto& c : columns)
            if (c.name == col) return &c;
        return nullptr;
    }
    bool hasColumn(const std::string& col) const {
        return find(col) != nullptr;
    }
    // Returns the name of the primary key column, or "" if none declared.
    // UPDATE/DELETE handlers use this to extract the row key for put/remove.
    std::string primaryKeyColumn() const {
        for (const auto& c : columns)
            if (c.primaryKey) return c.name;
        return std::string{};
    }
    bool hasPrimaryKey() const noexcept { return !primaryKeyColumn().empty(); }
};

// Phase 6.1: ALTER TABLE operations. A provider receives an AlterSpec and
// applies the change atomically. Operations are variant-style: only one
// of addColumn / dropColumn / renameTable is meaningful per spec. The
// provider rejects any other combinations with DbError::notSupported.
//
//   - addColumn: append a new column to the schema. Existing rows get
//                std::nullopt for the new column (NULL value).
//   - dropColumn: remove a column from the schema. Existing rows lose
//                 that field on the next read.
//   - renameTable: rename the table. The old name is unregistered, the
//                  new name is registered, with the same data.
struct AlterSpec {
    enum class Kind { AddColumn, DropColumn, RenameTable };
    Kind kind;
    std::string column;            // AddColumn: name+type of new col;
                                   // DropColumn: name of col to remove;
                                   // RenameTable: new table name.
    ColumnDef columnDef;           // AddColumn: full definition (used by
                                   //              describeTable).
                                   // DropColumn / RenameTable: ignored.
};

} // namespace opendb

#endif // OPENDB_SCHEMA_HPP
