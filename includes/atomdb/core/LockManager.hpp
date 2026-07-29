#ifndef ATOMDB_LOCK_MANAGER_HPP
#define ATOMDB_LOCK_MANAGER_HPP

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "atomdb/types/TxnId.hpp"

namespace atomdb {

enum class LockMode {
    Shared,
    Exclusive,
};

// Hash for ResourceKey so it can be used as an unordered_map key.
struct LockResourceHash {
    std::size_t operator()(const std::tuple<std::string, std::string>& key) const noexcept {
        std::hash<std::string> h;
        return h(std::get<0>(key)) ^ (h(std::get<1>(key)) << 1);
    }
};

// LockManager (spec §5.2, §5.4) — Shared/Exclusive locks with blocking
// semantics. v0.1 granularity was table-level; v0.2 (Phase 5 Item 6 inner-core
// upgrade) adds optional row-level granularity via ResourceKey. The user
// selected "block on lock with a wait queue" over "fail immediately", so
// `acquire()` blocks on a condition variable until the request is grantable,
// then returns. To support deadlock detection without recursion, `acquire`
// does *not* call into DeadlockDetector itself; the EngineLoop checks the
// wait-for graph separately via DeadlockDetector, which reads from this
// LockManager's waiter records.
//
// ResourceKey identifies a lockable resource:
//   - (table, "") -> the whole table (legacy table-level lock).
//   - (table, "<key>") -> a single row keyed by `<key>`.
//
// Spec §5.2 compatibility matrix (applied per resource):
//   Held-by-others \ Requested   Shared   Exclusive
//   None held                      Grant   Grant
//   Shared held                    Grant   Deny
//   Exclusive held                 Deny    Deny
//
// Self-held locks are always compatible (including upgrade from Shared to
// Exclusive by the same txn). Conflicting requests from *other* txns cause this
// caller to block until those txns release.
class LockManager {
public:
    using ResourceKey = std::tuple<std::string, std::string>; // (table, rowKey)

    static ResourceKey tableKey(const std::string& table) {
        return ResourceKey{table, std::string{}};
    }
    static ResourceKey rowKey(const std::string& table, const std::string& key) {
        return ResourceKey{table, key};
    }

    // Acquire a table-level lock (backwards-compatible shortcut).
    void acquire(TxnId txn, const std::string& table, LockMode mode) {
        acquireKey(txn, tableKey(table), mode);
    }

    // Acquire a lock on a specific resource (table or row).
    void acquireKey(TxnId txn, const ResourceKey& key, LockMode mode) {
        std::unique_lock<std::mutex> lk(mutex_);
        while (!grantableUnlocked(txn, key, mode)) {
            waiters_[key].push_back(Waiter{txn, mode});
            cv_.wait(lk);
            auto& w = waiters_[key];
            for (auto it = w.begin(); it != w.end(); ++it) {
                if (it->txn == txn) { w.erase(it); break; }
            }
        }
        grantUnlocked(txn, key, mode);
    }

    // Release every lock held by `txn` across all tables/rows, then notify
    // all blocked waiters.
    void release(TxnId txn) {
        std::vector<ResourceKey> affected;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto it = held_.begin(); it != held_.end(); ) {
                auto& holders = it->second.holders;
                bool removed = false;
                for (auto hi = holders.begin(); hi != holders.end(); ++hi) {
                    if (*hi == txn) { holders.erase(hi); removed = true; break; }
                }
                if (removed) {
                    affected.push_back(it->first);
                    if (holders.empty()) it = held_.erase(it);
                    else ++it;
                } else {
                    ++it;
                }
            }
            upgraders_.erase(txn);
        }
        if (!affected.empty()) cv_.notify_all();
    }

    // Release just the table-level lock on `table` held by `txn` if any.
    void release(TxnId txn, const std::string& table) {
        releaseKey(txn, tableKey(table));
    }

    // Release the lock on a specific resource held by `txn`, if any.
    void releaseKey(TxnId txn, const ResourceKey& key) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = held_.find(key);
            if (it != held_.end()) {
                auto& holders = it->second.holders;
                for (auto hi = holders.begin(); hi != holders.end(); ++hi) {
                    if (*hi == txn) { holders.erase(hi); changed = true; break; }
                }
                if (changed && holders.empty()) held_.erase(it);
            }
        }
        if (changed) cv_.notify_all();
    }

    // Read-only query for deadlock detection. Returns the TxnIds currently
    // holding any lock mode on `table` (table-level + all row-level).
    std::vector<TxnId> getHolders(const std::string& table) const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::unordered_set<TxnId> uniq;
        for (const auto& [key, lock] : held_) {
            if (std::get<0>(key) == table) {
                for (TxnId h : lock.holders) uniq.insert(h);
            }
        }
        return std::vector<TxnId>(uniq.begin(), uniq.end());
    }

    // Read-only query for deadlock detection. Returns the TxnIds currently
    // waiting (any mode) for a lock on `table`.
    std::vector<TxnId> getWaiters(const std::string& table) const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::unordered_set<TxnId> uniq;
        for (const auto& [key, list] : waiters_) {
            if (std::get<0>(key) != table) continue;
            for (const auto& w : list) uniq.insert(w.txn);
        }
        return std::vector<TxnId>(uniq.begin(), uniq.end());
    }

    // Read-only: returns a *representative* resource this txn is waiting on,
    // or an empty key if not waiting. Used by DeadlockDetector to start a
    // wait-chain walk. (Legacy: returns a table-level ResourceKey — the
    // detector only needs the table name.)
    ResourceKey waiterResource(TxnId txn) const {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& [key, list] : waiters_) {
            for (const auto& w : list) if (w.txn == txn) return key;
        }
        return ResourceKey{std::string{}, std::string{}};
    }

    // Back-compat helper: returns the table this txn is waiting on.
    std::string waiterTable(TxnId txn) const {
        return std::get<0>(waiterResource(txn));
    }

    bool isGranted(TxnId txn, const std::string& table) const {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& [key, lock] : held_) {
            if (std::get<0>(key) != table) continue;
            for (TxnId h : lock.holders) if (h == txn) return true;
        }
        return false;
    }

    bool isGrantedKey(TxnId txn, const ResourceKey& key) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = held_.find(key);
        if (it == held_.end()) return false;
        for (TxnId h : it->second.holders) if (h == txn) return true;
        return false;
    }

private:
    struct Lock {
        // Either a single Exclusive holder (exclusive mode) or a set of
        // Shared holders (shared mode). Mode discriminates which is active.
        LockMode mode = LockMode::Shared;
        std::vector<TxnId> holders;
    };
    struct Waiter {
        TxnId txn;
        LockMode mode;
    };

    // Precondition: caller holds mutex_.
    bool grantableUnlocked(TxnId txn, const ResourceKey& key, LockMode mode) const {
        auto it = held_.find(key);
        if (it == held_.end()) return true;
        // Self-held: txn already holds the lock. Always grantable (so a
        // holder can upgrade Shared -> Exclusive).
        for (TxnId h : it->second.holders) if (h == txn) return true;
        // Otherwise: only Shared-mode resources can accept concurrent Shared.
        if (mode == LockMode::Shared && it->second.mode == LockMode::Shared) return true;
        return false;
    }

    // Precondition: caller holds mutex_ AND grantableUnlocked returned true.
    void grantUnlocked(TxnId txn, const ResourceKey& key, LockMode mode) {
        auto it = held_.find(key);
        if (it == held_.end()) {
            Lock l;
            l.mode = mode;
            l.holders.push_back(txn);
            held_.emplace(key, std::move(l));
            return;
        }
        // Self-held: upgrade Shared -> Exclusive.
        bool alreadyHolder = false;
        for (TxnId h : it->second.holders) if (h == txn) { alreadyHolder = true; break; }
        if (alreadyHolder) {
            if (mode == LockMode::Exclusive && it->second.mode == LockMode::Shared) {
                it->second.mode = LockMode::Exclusive;
            }
            return;
        }
        // Adding a new holder. Convert Shared -> Exclusive: collapse all
        // existing shared holders into one slot, then set mode Exclusive.
        if (mode == LockMode::Exclusive) {
            it->second.holders.clear();
            it->second.mode = LockMode::Exclusive;
        }
        it->second.holders.push_back(txn);
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<ResourceKey, Lock, LockResourceHash> held_;
    std::unordered_map<ResourceKey, std::vector<Waiter>, LockResourceHash> waiters_;
    std::unordered_map<TxnId, std::string> upgraders_; // reserved for future use
};

} // namespace atomdb

#endif // ATOMDB_LOCK_MANAGER_HPP
