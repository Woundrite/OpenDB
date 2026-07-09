#ifndef ATOMDB_ISTORAGE_ENGINE_HPP
#define ATOMDB_ISTORAGE_ENGINE_HPP

#include <functional>
#include <optional>
#include <string>

#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/TxnId.hpp"
#include "atomdb/types/Value.hpp"

namespace atomdb {

// IStorageEngine: the back-end plugin contract (spec §4.2).
//
// Every concrete storage engine (In-Memory, Local B-Tree, Caching decorator,
// Sharded, ...) implements this interface. The core engine only ever holds an
// IStorageEngine* — never a subclass pointer directly — so composition (a
// Caching engine wrapping a Sharded engine, per spec §4.3) is invisible to the
// core: the outer engine just delegates calls inward.
//
// Each method is scoped to a TxnId so the storage engine can isolate writes
// performed in an uncommitted transaction from reads issued in another. The
// engine's own behavior is left to the implementer (Append-only versioned
// records for v0.1 per the user decision; the contract is agnostic).
//
// Two-phase commit lifecycle (spec §4.2):
//   prepare(TxnId) -> DbError    // an engine may flush to disk here if durable
//   commit(TxnId) -> DbError     // makes staged writes visible to later readers
//   abort(TxnId)  -> DbError     // discards all staged writes for this txn
// A purely in-memory engine implements prepare as a trivial success and commit
// as "stamp staged versions with visibleSeq". A future distributed backend can
// implement real two-phase commit without changing this contract.
class IStorageEngine {
public:
    virtual ~IStorageEngine() = default;

    // Point lookup of a record by key.
    //  - Returns the most recent committed, non-tombstone version visible to this
    //    txn, or std::nullopt if no such record exists.
    virtual std::optional<Tuple> get(TxnId txn,
                                       const std::string& table,
                                       const Value& key) = 0;

    // Insert or overwrite a record at `key`. Returns a DbError on failure
    // (or DbError with code Internal + empty message on success — sentinel).
    // The engine MAY auto-assign a key when the supplied row exposes no key
    // column (per the user's "auto if absent" decision); in that case the
    // assigned key is reflected in `row` before return.
    virtual DbError put(TxnId txn,
                        const std::string& table,
                        const Value& key,
                        const Tuple& row) = 0;

    // Delete a record at `key`. Append-only backends record a tombstone version;
    // future reads of this key (within or outside this txn) return std::nullopt.
    virtual DbError remove(TxnId txn,
                            const std::string& table,
                            const Value& key) = 0;

    // Scan every visible, non-tombstone record in a table, invoking the
    // callback once per row. Iterate in implementation-defined row order
    // (In-Memory uses std::map ordering over keys).
    virtual void scan(TxnId txn,
                      const std::string& table,
                      const std::function<void(const Tuple&)>& emit) = 0;

    // Two-phase commit lifecycle.
    virtual DbError prepare(TxnId txn) = 0;
    virtual DbError commit(TxnId txn) = 0;
    virtual DbError abort(TxnId txn) = 0;
};

} // namespace atomdb

#endif // ATOMDB_ISTORAGE_ENGINE_HPP
