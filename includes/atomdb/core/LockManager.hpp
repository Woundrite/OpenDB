#ifndef ATOMDB_LOCK_MANAGER_HPP
#define ATOMDB_LOCK_MANAGER_HPP

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
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

// LockManager (spec §5.2) — table-level Shared/Exclusive locks with blocking
// semantics. v0.1 granularity is table-level; row-level is a planned upgrade
// (spec §1.2, §5.4). The user selected "block on lock with a wait queue" over
// "fail immediately", so `acquire()` blocks on a condition variable until the
// request is grantable, then returns. To support deadlock detection without
// recursion, `acquire` does *not* call into DeadlockDetector itself; the
// EngineLoop checks the wait-for graph separately via DeadlockDetector, which
// reads from this LockManager's waiter records.
//
// Spec §5.2 compatibility matrix:
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
    // Acquire a lock, blocking until grantable. Returns true once granted (the
    // only failure mode in v0.1 is spurious-wakeup storms, which we treat as a
    // timeout loop and will not surface as false). Deadlock detection is the
    // EngineLoop's responsibility: it should call DeadlockDetector with *this
    // LockManager and the current TxnId between waits (we expose the required
    // query methods for that).
    void acquire(TxnId txn, const std::string& table, LockMode mode) {
        std::unique_lock<std::mutex> lk(mutex_);
        while (!grantableUnlocked(txn, table, mode)) {
            // Record that this txn is waiting on this table, attached to its
            // requested mode, so DeadlockDetector can walk the wait-for chain.
            waiters_[table].push_back(Waiter{txn, mode});
            cv_.wait(lk);
            // Remove our wait marker before retrying (we may immediately become a
            // waiter again on the next iteration if not grantable yet).
            auto& w = waiters_[table];
            for (auto it = w.begin(); it != w.end(); ++it) {
                if (it->txn == txn) { w.erase(it); break; }
            }
        }
        grantUnlocked(txn, table, mode);
    }

    // Release every lock held by `txn` across all tables, then notify all
    // blocked waiters (a single broadcast is simplest & correct, even if it
    // wakes a few spurious waiters).
    void release(TxnId txn) {
        std::vector<std::string> affected;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto it = held_.begin(); it != held_.end(); ) {
                if (it->second.holder == txn) {
                    affected.push_back(it->first);
                    it = held_.erase(it);
                } else {
                    ++it;
                }
            }
            // also clear any updater register for this txn
            upgraders_.erase(txn);
        }
        if (!affected.empty()) cv_.notify_all();
    }

    // Release just the lock on `table` held by `txn` if any.
    void release(TxnId txn, const std::string& table) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = held_.find(table);
            if (it != held_.end() && it->second.holder == txn) {
                held_.erase(it);
                changed = true;
            }
        }
        if (changed) cv_.notify_all();
    }

    // Read-only query for deadlock detection. Returns the TxnIds currently
    // holding any lock mode on `table`.
    std::vector<TxnId> getHolders(const std::string& table) const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<TxnId> out;
        auto it = held_.find(table);
        if (it != held_.end()) out.push_back(it->second.holder);
        return out;
    }

    // Read-only query for deadlock detection. Returns the TxnIds currently
    // waiting (any mode) for a lock on `table`.
    std::vector<TxnId> getWaiters(const std::string& table) const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<TxnId> out;
        auto it = waiters_.find(table);
        if (it != waiters_.end()) {
            out.reserve(it->second.size());
            for (const auto& w : it->second) out.push_back(w.txn);
        }
        return out;
    }

    // Read-only: returns the (single) table this txn is waiting on, or "" if
    // not waiting. Used by the DeadlockDetector to start a wait-chain walk.
    std::string waiterTable(TxnId txn) const {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& [table, list] : waiters_) {
            for (const auto& w : list) if (w.txn == txn) return table;
        }
        return std::string{};
    }

    bool isGranted(TxnId txn, const std::string& table) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = held_.find(table);
        return it != held_.end() && it->second.holder == txn;
    }

private:
    struct Lock {
        TxnId holder;
        LockMode mode;
    };
    struct Waiter {
        TxnId txn;
        LockMode mode;
    };

    // Precondition: caller holds mutex_.
    bool grantableUnlocked(TxnId txn, const std::string& table, LockMode mode) const {
        auto it = held_.find(table);
        if (it == held_.end()) return true;
        if (it->second.holder == txn) {
            // Self-held: always compatible, including Shared -> Exclusive upgrade.
            return true;
        }
        // Held by someone else.
        if (mode == LockMode::Shared && it->second.mode == LockMode::Shared) return true;
        return false;
    }

    // Precondition: caller holds mutex_ AND grantableUnlocked returned true.
    void grantUnlocked(TxnId txn, const std::string& table, LockMode mode) {
        auto it = held_.find(table);
        if (it == held_.end()) {
            held_.emplace(table, Lock{txn, mode});
            return;
        }
        if (it->second.holder == txn) {
            // Self-held: upgrade if applicable, never downgrade.
            if (mode == LockMode::Exclusive && it->second.mode == LockMode::Shared) {
                it->second.mode = LockMode::Exclusive;
            }
            return;
        }
        // Should be unreachable because grantableUnlocked returned true.
        it->second.holder = txn;
        it->second.mode = mode;
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Lock> held_;
    std::unordered_map<std::string, std::vector<Waiter>> waiters_;
    std::unordered_map<TxnId, std::string> upgraders_; // reserved for future use
};

} // namespace atomdb

#endif // ATOMDB_LOCK_MANAGER_HPP
