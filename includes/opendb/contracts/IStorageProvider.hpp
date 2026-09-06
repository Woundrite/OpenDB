#ifndef OPENDB_ISTORAGE_PROVIDER_HPP
#define OPENDB_ISTORAGE_PROVIDER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "opendb/types/DbError.hpp"
#include "opendb/types/Schema.hpp"
#include "opendb/types/Value.hpp"

namespace opendb {

// Forward declaration — composition: a provider owns an engine; it is not one.
class IStorageEngine;

// Capability flags returned by IStorageProvider::capabilities(). A provider
// declares what it can do; the core never hard-codes capabilities — it queries.
enum class StorageCapability : std::uint32_t {
    Durable         = 1u << 0,  // data survives process exit (LocalFileProvider)
    CrashDurable    = 1u << 1,  // data survives mid-write crash (reserved for WAL, Phase 5)
    RandomAccess    = 1u << 2,  // O(log n) or better get() by key
    OrderedScan     = 1u << 3,  // scan() emits rows in key order
    BlobSupport     = 1u << 4,  // accepts Value::Blob payloads
    TemporalSupport = 1u << 5,  // accepts Value::Date / Value::Timestamp
    Concurrent      = 1u << 6,  // provider is safe for concurrent threads (Phase 4 requirement)
    Networked       = 1u << 7,  // backing store is reachable over net (RemoteApi provider)
};

// Type vocabulary: the set of Value tags the provider understands natively.
// The core never invents types; DDL with a column whose declared type is NOT
// in `accepted` MUST be rejected by createTable with DbError::notSupported.
struct TypeVocabulary {
    std::vector<ValueType> accepted;
    // Maps each ValueType to the provider's native name (e.g. Int64 -> "BIGINT").
    std::unordered_map<ValueType, std::string> providerNativeName;
    // Per-type hard limit on bytes-on-wire. 0 = unlimited.
    std::unordered_map<ValueType, std::uint64_t> maxByteLength;
};

// IStorageProvider: the back-end plugin contract that owns the data path,
// capability declaration, lifecycle, and schema/DDL surface. This is the
// contract above the existing IStorageEngine (spec §4.2 extended in Phase 4).
//
// Composition model (per design decision: prefer composition over inheritance):
//   - A concrete provider (e.g. LocalFileStorageProvider) implements
//     IStorageProvider AND internally owns an IStorageEngine-shaped object.
//     It exposes that engine via `engine()`. The core (EngineDispatcher) is
//     constructed with `provider->engine()` and has no idea a provider exists.
//   - Decorator providers (Caching, Sharded) implement IStorageProvider and
//     hold another IStorageProvider*; they forward `engine()` to their inner
//     provider's engine (Caching) or expose a merged engine (Sharded).
//
// Threading: per the Phase 4 design the engine is multi-threaded. A provider
// declaring `Concurrent` in capabilities() MUST allow concurrent calls from
// multiple worker threads to its engine facet. The In-Memory engine already
// satisfies this (mutex-guarded); LocalFile must guarantee the same.
class IStorageProvider {
public:
    virtual ~IStorageProvider() = default;

    // ---- Identity / capability probing ---------------------------------------
    virtual std::string  name() const = 0;          // e.g. "local-file", "in-memory"
    virtual std::uint32_t capabilities() const = 0;  // bitmask of StorageCapability
    virtual TypeVocabulary typeVocabulary() const = 0;

    // ---- Lifecycle ----------------------------------------------------------
    // Opens the backing store. URI semantics are provider-defined:
    //   "in-memory://"               -> RAM only
    //   "file://./data/opendb.dat"   -> single-file B-Tree
    //   "sharded+file://./cfg.toml"  -> sharded provider wrapping file shards
    //   "cache+file://./data/atom.db"-> caching decorator wrapping file provider
    // Returns sentinel on success; DbError on failure (file-not-found, perm, etc.).
    virtual DbError open(const std::string& uri) = 0;
    virtual DbError close() = 0;
    virtual bool    isOpen() const noexcept = 0;

    // ---- Schema (DDL surface; owned by the provider, not the core) ----------
    // The provider stores schema metadata however it likes (in-core hash map
    // for InMemory; on metadata pages for LocalFile; via a remote schema
    // service for RemoteApi). Front-ends query describeTable() to validate
    // incoming INSERTs and to render schema introspection endpoints.
    virtual DbError createTable(const Schema& schema) = 0;
    virtual DbError dropTable(const std::string& name) = 0;
    virtual std::optional<Schema> describeTable(const std::string& name) const = 0;
    virtual std::vector<std::string> tables() const = 0;

    // ---- Phase 6.1: ALTER TABLE (DDL mutation) ------------------------------
    // Apply an AlterSpec to the named table. Atomically updates the schema
    // metadata + any on-disk representation the provider keeps. Implementers
    // are responsible for migrating existing rows if needed (e.g. default
    // value for AddColumn) and for accepting the change before returning.
    virtual DbError alterTable(const std::string& name, const AlterSpec& spec) = 0;

    // ---- Engine access ------------------------------------------------------
    // Returns the IStorageEngine facet held by this provider. The EngineDispatcher
    // is constructed with this pointer; the core is unaware of the provider layer.
    // Lifetime: bounded by the provider. Caller MUST NOT delete.
    virtual IStorageEngine* engine() = 0;
};

} // namespace opendb

#endif // OPENDB_ISTORAGE_PROVIDER_HPP
