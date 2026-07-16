#ifndef ATOMDB_SHARDED_STORAGE_ENGINE_HPP
#define ATOMDB_SHARDED_STORAGE_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atomdb/contracts/IStorageEngine.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/TxnId.hpp"
#include "atomdb/types/Value.hpp"

namespace atomdb {

// ShardedStorageEngine: IStorageEngine implementation that routes get/put/remove/scan
// to child engines based on a hash of the key's bytes.
//
// ponytail: Hash is sufficient for v0.1. Range/List would need a different
// routing scheme (boundary comparison or membership set) — out of scope here.
// Also: scan simply fans out and concatenates. OrderedScan across shards is
// approximation at best in a hash-sharded engine, but the callback contract
// doesn't carry ordering semantics, so we forward and let callers re-sort.
class ShardedStorageEngine : public IStorageEngine {
public:
    explicit ShardedStorageEngine(std::vector<IStorageEngine*> childEngines)
        : childEngines_(std::move(childEngines)) {}

    ~ShardedStorageEngine() override = default;

    // IStorageEngine overrides
    std::optional<Tuple> get(TxnId txn, const std::string& table, const Value& key) override {
        auto idx = routeByKey(key);
        if (!idx) return std::nullopt;
        return childEngines_[*idx]->get(txn, table, key);
    }

    DbError put(TxnId txn, const std::string& table, const Value& key, const Tuple& row) override {
        auto idx = routeByKey(key);
        if (!idx) return DbError::notFound("no shard to route to");
        return childEngines_[*idx]->put(txn, table, key, row);
    }

    DbError remove(TxnId txn, const std::string& table, const Value& key) override {
        auto idx = routeByKey(key);
        if (!idx) return DbError::notFound("no shard to route to");
        return childEngines_[*idx]->remove(txn, table, key);
    }

    void scan(TxnId txn, const std::string& table,
              const std::function<void(const Tuple&)>& emit) override {
        for (auto* e : childEngines_) {
            e->scan(txn, table, emit);
        }
    }

    DbError prepare(TxnId txn) override {
        for (auto* e : childEngines_) {
            auto r = e->prepare(txn);
            if (!r.isSentinel()) return r;
        }
        return DbError::sentinel();
    }

    DbError commit(TxnId txn) override {
        for (auto* e : childEngines_) {
            auto r = e->commit(txn);
            if (!r.isSentinel()) return r;
        }
        return DbError::sentinel();
    }

    DbError commit(TxnId txn, std::uint64_t visibleSeq) override {
        for (auto* e : childEngines_) {
            auto r = e->commit(txn, visibleSeq);
            if (!r.isSentinel()) return r;
        }
        return DbError::sentinel();
    }

    DbError abort(TxnId txn) override {
        for (auto* e : childEngines_) {
            e->abort(txn);
        }
        return DbError::sentinel();
    }

private:
    std::vector<IStorageEngine*> childEngines_;

    std::optional<std::size_t> routeByKey(const Value& key) const {
        if (childEngines_.empty()) return std::nullopt;
        if (childEngines_.size() == 1) return std::size_t{0};

        // FNV-1a hash of the key's encoded bytes
        std::size_t h = 1469598103934665603ull;
        for (std::uint8_t b : key.toBytes()) {
            h ^= b;
            h *= 1099511628211ull;
        }
        return h % childEngines_.size();
    }
};

} // namespace atomdb

#endif // ATOMDB_SHARDED_STORAGE_ENGINE_HPP