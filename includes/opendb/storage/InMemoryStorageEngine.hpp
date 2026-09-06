#ifndef OPENDB_IN_MEMORY_STORAGE_ENGINE_HPP
#define OPENDB_IN_MEMORY_STORAGE_ENGINE_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "opendb/contracts/IStorageEngine.hpp"
#include "opendb/types/Column.hpp"
#include "opendb/types/DbError.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/TxnId.hpp"
#include "opendb/types/Value.hpp"

namespace opendb {

// InMemoryStorageEngine: the v0.1 storage backend (spec §4.2).
//
// Per the user's design decision this engine stores rows as **append-only
// versioned records**: every `put` appends a new version, every `remove` writes
// a tombstone marker. Reads walk the version chain newest-to-oldest, returning
// the newest visible (committed) non-tombstone version. This is the basis for
// MVCC in a later milestone; for v0.1, the visibility rule is:
//
//   A version is visible to txn T at read time iff:
//     (a) it was staged within T (its commitSeq == 0 and txnId == T), or
//     (b) it has been committed (commitSeq > 0 AND commitSeq <= visible_seq_),
//         AND was authored by a txn other than T. (A txn cannot see globals
//         it staged but not yet committed; only the next reader sees them.)
//
// The TransactionManager's visibleSeq is fetched by the EngineLoop and passed
// in via `commit(TxnId, visibleSeq)` so the engine can stamp staged versions.
// `prepare(TxnId)` is always success (no durable round-trip in v0.1).
// `abort(TxnId)` erases all staged-but-uncommitted versions for that txn.
//
// INSERTion without an explicit key (per the user's "auto if absent, explicit
// otherwise" decision): the caller signals "auto-assign me" by passing an
// empty Value (Value::null()). The engine assigns an Int64 Value from a
// per-table monotonic counter and writes it back into the row's "_id" column
// (for caller inspection). If the row already contains an "_id" column with
// a non-null value, that value is used as the key verbatim.
class InMemoryStorageEngine : public IStorageEngine {
public:
    InMemoryStorageEngine() = default;

    std::optional<Tuple> get(TxnId txn,
                               const std::string& table,
                               const Value& key) override {
        const std::lock_guard<std::mutex> lk(mutex_);
        auto tbl_it = tables_.find(table);
        if (tbl_it == tables_.end()) return std::nullopt;
        auto row_it = tbl_it->second.find(key);
        if (row_it == tbl_it->second.end()) return std::nullopt;
        for (auto v_it = row_it->second.rbegin(); v_it != row_it->second.rend(); ++v_it) {
            if (isVisibleUnlocked(txn, *v_it)) {
                if (v_it->tombstone) return std::nullopt;
                return v_it->payload;
            }
        }
        return std::nullopt;
    }

    DbError put(TxnId txn,
                const std::string& table,
                const Value& key,
                const Tuple& row) override {
        const std::lock_guard<std::mutex> lk(mutex_);
        Value effective_key = key;
        Tuple stored_row = row;
        if (effective_key.isNull()) {
            // Auto-assign a per-table monotonic key.
            std::uint64_t next = ++auto_id_[table];
            effective_key = Value::int64(static_cast<std::int64_t>(next));
            stored_row.set("_id", effective_key);
        }
        Version v;
        v.commitSeq = 0; // staged (uncommitted)
        v.txnId = txn.value();
        v.payload = stored_row;
        v.tombstone = false;
        tables_[table][effective_key].push_back(std::move(v));
        return DbError::sentinel();
    }

    DbError remove(TxnId txn,
                    const std::string& table,
                    const Value& key) override {
        const std::lock_guard<std::mutex> lk(mutex_);
        Version v;
        v.commitSeq = 0;
        v.txnId = txn.value();
        v.tombstone = true;
        // payload stays empty for tombstones
        tables_[table][key].push_back(std::move(v));
        return DbError::sentinel();
    }

    void scan(TxnId txn,
              const std::string& table,
              const std::function<void(const Tuple&)>& emit) override {
        const std::lock_guard<std::mutex> lk(mutex_);
        auto tbl_it = tables_.find(table);
        if (tbl_it == tables_.end()) return;
        // Walk keys in order (std::map<Value, ...> sorts via Value::compare via
        // Compare-less-than <=> through operator< which is derived from compare).
        for (const auto& [key, versions] : tbl_it->second) {
            for (auto v_it = versions.rbegin(); v_it != versions.rend(); ++v_it) {
                if (isVisibleUnlocked(txn, *v_it)) {
                    if (v_it->tombstone) break; // newest visible version is a tombstone -> row not visible
                    emit(v_it->payload);
                    break;
                }
            }
        }
    }

    DbError prepare(TxnId txn) override {
        (void)txn;
        return DbError::sentinel(); // trivial success for an in-memory engine
    }

    // Issue a commit for this txn, stamping all staged versions with the
    // freshly-advanced visibleSeq. The EngineLoop passes the value returned
    // by TransactionManager::visibleSeq() AFTER calling commitTxn(txn) on
    // the TransactionManager.
    DbError commit(TxnId txn, std::uint64_t visibleSeq) override {
        const std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [table_name, rows] : tables_) {
            for (auto& [key, versions] : rows) {
                for (auto& v : versions) {
                    if (v.txnId == txn.value() && v.commitSeq == 0) {
                        v.commitSeq = visibleSeq;
                    }
                }
            }
        }
        visible_seq_ = std::max(visible_seq_, visibleSeq);
        return DbError::sentinel();
    }

    // Convenience commit for callers that don't have a visibleSeq — generates
    // one locally so this method remains self-contained. The EngineLoop should
    // prefer the 2-arg form when TransactionManager is coordinating.
    DbError commit(TxnId txn) override {
        std::uint64_t seq = ++internal_seq_;
        return commit(txn, seq);
    }

    DbError abort(TxnId txn) override {
        const std::lock_guard<std::mutex> lk(mutex_);
        // Snapshot table names — we may erase tables from `tables_` below.
        std::vector<std::string> table_names;
        table_names.reserve(tables_.size());
        for (const auto& [name, _] : tables_) table_names.push_back(name);

        for (const auto& table_name : table_names) {
            auto tbl_it = tables_.find(table_name);
            if (tbl_it == tables_.end()) continue;
            auto& rows = tbl_it->second;
            for (auto row_it = rows.begin(); row_it != rows.end(); ) {
                auto& versions = row_it->second;
                auto erase_from = std::remove_if(versions.begin(), versions.end(),
                    [&](const Version& v) {
                        return v.txnId == txn.value() && v.commitSeq == 0;
                    });
                versions.erase(erase_from, versions.end());
                if (versions.empty()) {
                    row_it = rows.erase(row_it);
                    continue;
                }
                ++row_it;
            }
            // If the table is now empty, drop it so future rowCount == 0.
            if (rows.empty()) {
                tables_.erase(tbl_it);
                auto_id_.erase(table_name);
            }
        }
        return DbError::sentinel();
    }

    // Diagnostics (used by tests).
    std::size_t tableCount() const {
        const std::lock_guard<std::mutex> lk(mutex_);
        return tables_.size();
    }
    std::size_t rowCount(const std::string& table) const {
        const std::lock_guard<std::mutex> lk(mutex_);
        auto it = tables_.find(table);
        if (it == tables_.end()) return 0;
        return it->second.size();
    }

private:
    struct Version {
        std::uint64_t commitSeq = 0;  // 0 means staged (uncommitted); >0 once committed
        std::uint64_t txnId = 0;      // the txn that wrote this version
        Tuple payload;
        bool tombstone = false;
    };

// Precondition: caller holds mutex_.
    bool isVisibleUnlocked(TxnId txn, const Version& v) const {
        if (v.commitSeq == 0) {
            // Staged by some txn; visible only if staged by this txn (read-your-writes).
            return v.txnId == txn.value();
        }
        // Committed by some txn. In v0.1 a committed version is globally visible
        // to all later reads (no snapshot isolation yet). The visibleSeq cut
        // is applied here for full MVCC.
        return v.commitSeq <= visible_seq_;
    }

mutable std::mutex mutex_;
    std::unordered_map<std::string, std::map<Value, std::vector<Version>>> tables_;
    std::unordered_map<std::string, std::uint64_t> auto_id_;
    std::uint64_t internal_seq_ = 0;
    std::uint64_t visible_seq_ = 0;
};

} // namespace opendb

#endif // OPENDB_IN_MEMORY_STORAGE_ENGINE_HPP

