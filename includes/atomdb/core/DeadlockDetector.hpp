#ifndef ATOMDB_DEADLOCK_DETECTOR_HPP
#define ATOMDB_DEADLOCK_DETECTOR_HPP

#include <optional>

#include "atomdb/core/LockManager.hpp"
#include "atomdb/types/TxnId.hpp"

namespace atomdb {

// DeadlockDetector (spec §5.3) — single wait-chain traversal.
//
// Algorithm:
//   Starting from `origin` (the txn that called acquire() and is now waiting),
//   read LockManager::waiterTable(origin) to find the table X it's waiting on.
//   For each current holder H of X, check whether H is also waiting on some
//   table Y (waiterTable(H)). If yes, recurse: the new search root is H. If
//   the recursion encounters `origin` again, a cycle exists — return origin as
//   the victim (spec §5.3 "abort the transaction that triggered detection").
//   If we reach a holder that is not waiting, or run out of holders, return
//   nullopt — no cycle on this chain.
//
// Known limitation (spec §5.3): single wait-chain traversal explores one
// direct path from `origin`. It correctly identifies simple two-party cycles
// (T1 <-> T2) and chains that loop back to origin. A complete multi-txn cycle
// detector should explore ALL outgoing wait edges per transaction (a full graph
// traversal), not just the one currently being followed. Planned upgrade.
//
// The detector does NOT call LockManager::acquire (which would block); it reads
// only the in-memory wait-for records via the public query methods.
class DeadlockDetector {
public:
    // Construct with a reference to the LockManager containing the wait-for
    // records to traverse. References must outlive the detector.
    explicit DeadlockDetector(LockManager& lm) : lock_manager_(lm) {}

    // Returns the TxnId that should be aborted (origin) if a cycle was found,
    // or std::nullopt if no cycle is reachable from origin's wait chain. Limit
    // chain depth to break out of pathological long walks (the walk is
    // effectively bounded by the number of concurrent txns; cap defensively).
    std::optional<TxnId> detectCycle(TxnId origin) const {
        // First hop: origin waits on some table X.
        std::string table = lock_manager_.waiterTable(origin);
        if (table.empty()) return std::nullopt;
        // Visit up to a fixed cap to prohibit unbounded recursion in the
        // (extremely unlikely) case of a non-terminating chain.
        constexpr int kMaxHops = 1024;
        for (int i = 0; i < kMaxHops; ++i) {
            for (TxnId holder : lock_manager_.getHolders(table)) {
                if (holder == origin) return origin;                        // cycle closed
                std::string waits_on = lock_manager_.waiterTable(holder);
                if (waits_on.empty()) continue;                             // holder runs cleanly
                table = waits_on;
                // Continue the single-chain walk from this new table.
                goto next_hop;
            }
            return std::nullopt; // no holders of current table are themselves waiting
        next_hop:;
        }
        return std::nullopt;
    }

private:
    LockManager& lock_manager_;
};

} // namespace atomdb

#endif // ATOMDB_DEADLOCK_DETECTOR_HPP
