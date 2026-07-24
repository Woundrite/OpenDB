#ifndef ATOMDB_SHARDED_STORAGE_ENGINE_HPP
#define ATOMDB_SHARDED_STORAGE_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "atomdb/contracts/IStorageEngine.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Schema.hpp"
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
        // v1: `get` always routes by key hash. Range/List partitions where the
        // partition column is not the supplied key fall back to a fan-out
        // scan via the `scan` API. For partition column == key, callers
        // should pass the partition-key value (which the upper layers
        // guarantee when using `WHERE partition_col = ?`).
        auto idx = routeByKey(key);
        if (!idx) return std::nullopt;
        return childEngines_[*idx]->get(txn, table, key);
    }

    DbError put(TxnId txn, const std::string& table, const Value& key, const Tuple& row) override {
        // ponytail: try policy-driven routing first (works for Hash on any
        // key + Range/List when partition column is in the row). Fall back
        // to key hash if policy doesn't pin a shard.
        auto policy = policyFor(table);
        auto policy_idx = routeByPolicy(policy, row);
        std::size_t idx = 0;
        if (policy_idx) {
            idx = *policy_idx;
        } else if (auto k_idx = routeByKey(key)) {
            idx = *k_idx;
        } else {
            return DbError::notFound("no shard to route to");
        }
        return childEngines_[idx]->put(txn, table, key, row);
    }

    DbError remove(TxnId txn, const std::string& table, const Value& key) override {
        // ponytail: Range/List by partition column ≠ key — the upper layer
        // is responsible for erasing across all shards via scan + remove.
        // Here we route by key hash, which matches Range/List when partition
        // column == key.
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

    // ponytail: ShardedStorageProvider installs the PartitionPolicy for each
    // table as it creates them. Without this map, routing falls back to hash
    // for every table regardless of declared policy. Mutex is fine — set
    // rarely, read on every put/get/scan.
    void installPartitionPolicy(const std::string& table, const PartitionPolicy& policy) {
        const std::lock_guard<std::mutex> lk(policy_mu_);
        policies_[table] = policy;
    }

    void clearPartitionPolicy(const std::string& table) {
        const std::lock_guard<std::mutex> lk(policy_mu_);
        policies_.erase(table);
    }

private:
    std::vector<IStorageEngine*> childEngines_;
    std::unordered_map<std::string, PartitionPolicy> policies_;
    mutable std::mutex policy_mu_;

    PartitionPolicy policyFor(const std::string& table) const {
        const std::lock_guard<std::mutex> lk(policy_mu_);
        auto it = policies_.find(table);
        if (it == policies_.end()) return PartitionPolicy{}; // unpartitioned fallback
        return it->second;
    }

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

    std::optional<std::size_t> routeByPolicy(const PartitionPolicy& policy,
                                              const Tuple& row) const {
        if (policy.kind == PartitionPolicy::Kind::None) {
            return std::nullopt;
        }
        if (childEngines_.empty()) return std::nullopt;
        std::size_t n = childEngines_.size();

        auto val = row.maybeGet(policy.column);
        if (!val) {
            // Missing partition key → unrouted; fall back to hash.
            return std::nullopt;
        }

        switch (policy.kind) {
            case PartitionPolicy::Kind::Hash: {
                if (policy.shardCount == 0) return std::nullopt;
                std::size_t h = 1469598103934665603ull;
                for (std::uint8_t b : val->toBytes()) {
                    h ^= b;
                    h *= 1099511628211ull;
                }
                return h % n;
            }
            case PartitionPolicy::Kind::Range: {
                // boundaries has shardCount+1 entries; shard i covers
                // [boundaries[i], boundaries[i+1]).
                if (policy.boundaries.size() != n + 1) return std::nullopt;
                for (std::size_t i = 0; i < n; ++i) {
                    if (val->compare(policy.boundaries[i])   != std::strong_ordering::less &&
                        val->compare(policy.boundaries[i+1]) == std::strong_ordering::less) {
                        return i;
                    }
                }
                return std::nullopt;
            }
            case PartitionPolicy::Kind::List: {
                if (policy.lists.size() != n) return std::nullopt;
                for (std::size_t i = 0; i < n; ++i) {
                    for (const auto& member : policy.lists[i]) {
                        if (*val == member) return i;
                    }
                }
                return std::nullopt;
            }
            case PartitionPolicy::Kind::None:
                return std::nullopt;
        }
        return std::nullopt;
    }
};

} // namespace atomdb

#endif // ATOMDB_SHARDED_STORAGE_ENGINE_HPP