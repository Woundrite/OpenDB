#ifndef ATOMDB_CACHING_STORAGE_ENGINE_HPP
#define ATOMDB_CACHING_STORAGE_ENGINE_HPP

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "atomdb/contracts/IStorageEngine.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/TxnId.hpp"
#include "atomdb/types/Value.hpp"

namespace atomdb {

// ponytail: engine decorator — caches the latest visible (committed) version
// of each (table, key). Staged/visible-by-owning-txn reads bypass the cache
// because the inner engine is the only place that honors visibility. Writes
// invalidate the cache entry; scan forwards unconditionally.
//
// Limits: single-threaded by contract (we add a mutex). No LRU — pushed back
// to whoever profiles a real workload.
class CachingStorageEngine final : public IStorageEngine {
public:
    explicit CachingStorageEngine(IStorageEngine* inner) : inner_(inner) {}

    std::size_t cacheSize() const {
        const std::lock_guard<std::mutex> lk(mu_);
        return cache_.size();
    }

    std::optional<Tuple> get(TxnId txn,
                             const std::string& table,
                             const Value& key) override {
        // Cache only stores results for the *committed* read path (TxnId{0}
        // sentinel = "not a real txn"). Any other txn has staged writes the
        // inner engine must evaluate per-row — skip the cache.
        if (txn.value() == 0) {
            std::string ck = makeKey(table, key);
            {
                const std::lock_guard<std::mutex> lk(mu_);
                auto it = cache_.find(ck);
                if (it != cache_.end()) return it->second;
            }
            auto v = inner_->get(txn, table, key);
            const std::lock_guard<std::mutex> lk(mu_);
            cache_[std::move(ck)] = v;
            return v;
        }
        return inner_->get(txn, table, key);
    }

    DbError put(TxnId txn,
                const std::string& table,
                const Value& key,
                const Tuple& row) override {
        auto e = inner_->put(txn, table, key, row);
        if (e.isSentinel()) invalidate(table, key);
        return e;
    }

    DbError remove(TxnId txn,
                   const std::string& table,
                   const Value& key) override {
        auto e = inner_->remove(txn, table, key);
        if (e.isSentinel()) invalidate(table, key);
        return e;
    }

    void scan(TxnId txn,
              const std::string& table,
              const std::function<void(const Tuple&)>& emit) override {
        inner_->scan(txn, table, emit);
    }

    DbError prepare(TxnId txn) override { return inner_->prepare(txn); }
    DbError commit(TxnId txn) override { return inner_->commit(txn); }
    DbError commit(TxnId txn, std::uint64_t vs) override {
        return inner_->commit(txn, vs);
    }
    DbError abort(TxnId txn) override { return inner_->abort(txn); }

private:
    // ponytail: composite string key; table rarely changes, value cheap to render.
    // Avoids a dedicated std::hash<Value>; toString() is O(key size) which is
    // what you'd pay fingerprinting anyway.
    std::string makeKey(const std::string& table, const Value& key) const {
        std::string out;
        out.reserve(table.size() + 4 + key.toString().size());
        out.append(table);
        out.append("|->", 3);
        out.append(key.toString());
        return out;
    }

    void invalidate(const std::string& table, const Value& key) {
        const std::lock_guard<std::mutex> lk(mu_);
        cache_.erase(makeKey(table, key));
    }

    IStorageEngine* inner_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::optional<Tuple>> cache_;
};

} // namespace atomdb

#endif // ATOMDB_CACHING_STORAGE_ENGINE_HPP
