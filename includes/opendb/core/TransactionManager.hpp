#ifndef OPENDB_TRANSACTION_MANAGER_HPP
#define OPENDB_TRANSACTION_MANAGER_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "opendb/types/TxnId.hpp"

namespace opendb {

// TransactionManager (spec §5.1). Assigns monotonic TxnIds, tracks per-txn
// state machine (Active -> Committed | Aborted), and exposes a monotonically
// advancing `visibleSeq` used by storage engines that implement version-based
// visibility (In-Memory's "newest committed non-tombstone version" walk per
// user decision append-only versioned records).
//
// The TransactionManager knows nothing about locks or storage — by spec §5.1
// "Transactions, locks, and deadlock detection are coupled to each other by
// nature" but the manager only models *state*; orchestration lives in EngineLoop.
class TransactionManager {
public:
    enum class State {
        Active,
        Committed,
        Aborted,
        NotFound,  // TxnId was not allocated by this manager (or has been pruned).
    };

    TransactionManager() = default;

    // Allocate the next TxnId. TxnIds start at 1 (Txn0 reserved as the
    // "auto-commit / no-txn" sentinel sometimes used as a default).
    TxnId beginTxn() {
        const std::lock_guard<std::mutex> lk(mutex_);
        std::uint64_t next = ++next_id_;
        State s = State::Active;
        if (next == 1) s = State::Active; // first txn starts Active
        states_[next] = s;
        return TxnId{next};
    }

    // Mark a txn committed and bump the global commit sequence. The storage
    // engine reads visibleSeq() to learn the next commitSeq to stamp onto the
    // versions being committed.
    bool commitTxn(const TxnId txn) {
        const std::lock_guard<std::mutex> lk(mutex_);
        auto it = states_.find(txn.value());
        if (it == states_.end() || it->second != State::Active) return false;
        it->second = State::Committed;
        ++commit_seq_;
        return true;
    }

    bool abortTxn(const TxnId txn) {
        const std::lock_guard<std::mutex> lk(mutex_);
        auto it = states_.find(txn.value());
        if (it == states_.end() || it->second != State::Active) return false;
        it->second = State::Aborted;
        return true;
    }

    State getState(const TxnId txn) const {
        const std::lock_guard<std::mutex> lk(mutex_);
        auto it = states_.find(txn.value());
        if (it == states_.end()) return State::NotFound;
        return it->second;
    }

    // Monotonically increasing number incremented once per committed txn. The
    // In-Memory engine uses this as the commitSeq stamped onto newly-committed
    // versions; visibleSeq() at commit time is the value the new version
    // receives. Read this AFTER TransactionManager::commitTxn returns true.
    std::uint64_t visibleSeq() const {
        const std::lock_guard<std::mutex> lk(mutex_);
        return commit_seq_;
    }

private:
    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 0;
    std::uint64_t commit_seq_ = 0;
    std::unordered_map<std::uint64_t, State> states_;
};

} // namespace opendb

#endif // OPENDB_TRANSACTION_MANAGER_HPP
