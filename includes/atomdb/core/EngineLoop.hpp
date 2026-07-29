#ifndef ATOMDB_ENGINE_LOOP_HPP
#define ATOMDB_ENGINE_LOOP_HPP

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "atomdb/contracts/ICommandSource.hpp"
#include "atomdb/contracts/IStorageEngine.hpp"
#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/storage/InMemoryStorageEngine.hpp"
#include "atomdb/types/Command.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Result.hpp"
#include "atomdb/types/TxnId.hpp"

namespace atomdb {

// EngineLoop (spec §2.2). The non-pluggable core orchestrator that pulls
// Commands from an ICommandSource, runs each through the lock/handler/commit
// pipeline, and presents a ResultSet or DbError back to the source.
//
// Per spec §1.1 the core never depends on the edges; this class holds an
// ICommandSource* and an IStorageEngine* and is unaware of how input arrives
// or how data is stored beyond the contract surface.
//
// Dispatch summary (see spec §2.2 pipeline):
//   - If Command carries a TxnId, reuse it; else beginTxn (auto-commit mode).
//   - Determine lock mode (Select=Shared, anything else=Exclusive).
//   - Acquire the lock (blocking with deadlock detection interleave).
//   - Dispatch to the handler (Select / Insert / Update / Delete).
//   - Auto-commit cmds: prepare + commit + release locks; open-txn cmds: just
//     run the handler and leave the locks held (EngineLoop does not store open
//     txns between calls — they implicitly close when the source sends a new
//     Command without a TxnId, since each dispatched command in a v0.1 single-
//     threaded loop ends auto-commit).
class EngineLoop {
public:
    EngineLoop(ICommandSource& source,
                IStorageEngine& storage,
                TransactionManager& txn_mgr,
                LockManager& lock_mgr,
                DeadlockDetector& deadlock)
        : source_(source), storage_(storage),
          txn_mgr_(txn_mgr), lock_mgr_(lock_mgr), deadlock_(deadlock) {}

    // Run the main loop until the source signals EOF. Returns void; any error
    // produced within a single iteration is presented to the source back via
    // present(DbError), and the loop continues (per spec §5.4 "fails immediately
    // rather than blocking" — but v0.1 lock conflicts block; accept a deadlock
    // DbError and continue).
    void run() {
        for (;;) {
            auto opt = source_.nextCommand();
            if (!opt) return;
            dispatch(*opt);
        }
    }

private:
    void dispatch(const Command& cmd) {
        // Reuse or begin.
        const bool auto_commit = !cmd.txnId.has_value();
        TxnId txn = cmd.txnId.value_or(txn_mgr_.beginTxn());

        LockMode mode = (cmd.type == CommandType::Select) ? LockMode::Shared
                                                          : LockMode::Exclusive;
        // We use an interleaving pattern with the deadlock detector. The proper
        // fix (spec §2.2, Phase 5): spawn acquire on a worker thread; recheck
        // the wait-for graph on every wake. For v0.1 we make a single attempt
        // to detect a cycle *before* blocking. If the wait-for graph closes
        // back to this txn at the moment we call detectCycle, abort now.
        // Otherwise block via acquire until granted. (In a real blocking
        // scheduler we'd recheck the graph on every wake; for v0.1 the single
        // precheck detects the most common deterministic deadlock.)
        // Note: this is best-effort; the LockManager's acquire blocks the thread
        // directly via condition_variable.
        std::optional<TxnId> cycle_victim = deadlock_.detectCycle(txn);
        if (cycle_victim.has_value()) {
            txn_mgr_.abortTxn(*cycle_victim);
            lock_mgr_.release(*cycle_victim);
            source_.present(DbError::deadlock("cycle detected for " + txn.toString()));
            return;
        }

        // Acquire the lock. Blocks until granted. Could cause a race window
        // where a parallel txn starts a new cycle in between our check and
        // this acquire; for v0.1 the single-threaded loop accepts this.
        lock_mgr_.acquire(txn, cmd.table, mode);

        // Dispatch by command type.
        std::optional<ResultSet> result_opt;
        std::optional<DbError>    error_opt;

        switch (cmd.type) {
            case CommandType::Select: {
                ResultSet rs;
                rs.success = true;

                // Collect first: ORDER BY/LIMIT/OFFSET need the full filtered
                // row set in memory before presentation.
                std::vector<Tuple> matched;
                storage_.scan(txn, cmd.table, [&](const Tuple& row) {
                    if (!cmd.where.has_value() || cmd.where->evaluate(row)) {
                        if (cmd.projections.empty() ||
                            (cmd.projections.size() == 1 && cmd.projections[0] == "*")) {
                            matched.push_back(row);
                        } else {
                            Tuple projected;
                            for (const auto& pc : cmd.projections) {
                                auto v = row.maybeGet(pc);
                                if (v) projected.set(pc, std::move(*v));
                            }
                            matched.push_back(std::move(projected));
                        }
                    }
                });

                // ponytail: ORDER BY happens here, in-memory sort on the
                // collected rows. Lets compare() resolve Nulls/Mixed numeric
                // types consistently with the SQL semantics.
                if (!cmd.orderBy.empty()) {
                    std::vector<OrderBySpec> specs = cmd.orderBy;
                    std::sort(matched.begin(), matched.end(),
                              [&specs](const Tuple& a, const Tuple& b) {
                                  for (const auto& s : specs) {
                                      auto av = a.maybeGet(s.column);
                                      auto bv = b.maybeGet(s.column);
                                      if (!av && !bv) continue;
                                      if (!av) return s.direction == SortDirection::Asc;
                                      if (!bv) return s.direction != SortDirection::Asc;
                                      auto cmp = av->compare(*bv);
                                      if (cmp == std::strong_ordering::equal) continue;
                                      if (s.direction == SortDirection::Asc) {
                                          return cmp == std::strong_ordering::less;
                                      }
                                      return cmp == std::strong_ordering::greater;
                                  }
                                  return false;
                              });
                }

                std::size_t start = cmd.offset.value_or(0);
                std::size_t end   = matched.size();
                if (cmd.limit.has_value()) {
                    end = std::min(end, start + *cmd.limit);
                }
                if (start >= matched.size()) start = matched.size();
                if (end < start) end = start;
                for (std::size_t i = start; i < end; ++i) {
                    rs.rows.push_back(std::move(matched[i]));
                }
                result_opt = std::move(rs);
                break;
            }
            case CommandType::Insert: {
                // Per user decision: auto if absent, explicit otherwise.
                // The row's "_id" column (if present and non-null) is the key;
                // otherwise pass Value::null() and let the engine auto-assign.
                Value key = Value::null();
                Tuple row;
                if (cmd.values) {
                    row = *cmd.values;
                    if (row.has("_id")) {
                        Value id = row.get("_id");
                        if (!id.isNull()) key = std::move(id);
                    }
                }
                DbError err = storage_.put(txn, cmd.table, key, row);
                if (err.isSentinel()) {
                    // Success: emit a quiet OK so the REPL feedback is consistent.
                    ResultSet rs;
                    rs.success = true;
                    result_opt = std::move(rs);
                } else {
                    error_opt = err;
                }
                break;
            }
            case CommandType::Update: {
                if (!cmd.where) { error_opt = DbError::internal("Update requires WHERE"); break; }
                if (!cmd.values) { error_opt = DbError::internal("Update requires values"); break; }
                std::vector<Value> keysToUpdate;
                storage_.scan(txn, cmd.table, [&](const Tuple& row) {
                    if (cmd.where->evaluate(row)) {
                        auto pkOpt = row.maybeGet("_id");
                        if (pkOpt) keysToUpdate.push_back(*pkOpt);
                    }
                });
                bool any = false;
                ResultSet rs;
                for (const auto& k : keysToUpdate) {
                    Tuple row = *cmd.values;
                    row.set("_id", k);
                    auto err = storage_.put(txn, cmd.table, k, row);
                    if (!err.isSentinel()) { error_opt = err; break; }
                    any = true;
                }
                if (!error_opt && !any) error_opt = DbError::notFound("no matching rows to update");
                else if (!error_opt) rs = ResultSet(true, {});
                result_opt = std::move(rs);
                break;
            }
            case CommandType::Delete: {
                if (!cmd.where) { error_opt = DbError::internal("Delete requires WHERE"); break; }
                std::vector<Value> keysToDelete;
                storage_.scan(txn, cmd.table, [&](const Tuple& row) {
                    if (cmd.where->evaluate(row)) {
                        auto pkOpt = row.maybeGet("_id");
                        if (pkOpt) keysToDelete.push_back(*pkOpt);
                    }
                });
                bool any = false;
                ResultSet rs;
                for (const auto& k : keysToDelete) {
                    auto err = storage_.remove(txn, cmd.table, k);
                    if (!err.isSentinel()) { error_opt = err; break; }
                    any = true;
                }
                if (!error_opt && !any) error_opt = DbError::notFound("no matching rows to delete");
                else if (!error_opt) rs = ResultSet(true, {});
                result_opt = std::move(rs);
                break;
            }
        }

        // Auto-commit lifecycle.
        if (auto_commit) {
            if (result_opt.has_value()) {
                DbError err = storage_.prepare(txn);
                if (err.message().empty()) {
                    txn_mgr_.commitTxn(txn);
                    std::uint64_t visible_seq = txn_mgr_.visibleSeq();
                    storage_.commit(txn, visible_seq);
                } else {
                    error_opt = err;
                }
            } else if (error_opt.has_value()) {
                storage_.abort(txn);
                txn_mgr_.abortTxn(txn);
            }
            lock_mgr_.release(txn);
        }

        // Present.
        if (result_opt.has_value()) {
            const ResultSet& rs = *result_opt;
            // Write commands (Insert/Update/Delete) come through with an empty
            // ResultSet — emit a one-liner [OK] for them instead of "(no rows)".
            if (rs.rows.empty() && (cmd.type == CommandType::Insert ||
                                     cmd.type == CommandType::Update ||
                                     cmd.type == CommandType::Delete)) {
                source_.present(ResultSet(true, {}));
                return;
            }
            source_.present(*result_opt);
        } else if (error_opt.has_value()) {
            source_.present(*error_opt);
        } else {
            source_.present(DbError::internal("dispatch produced no result or error"));
        }
    }

    ICommandSource& source_;
    IStorageEngine& storage_;
    TransactionManager& txn_mgr_;
    LockManager& lock_mgr_;
    DeadlockDetector& deadlock_;
};

} // namespace atomdb

#endif // ATOMDB_ENGINE_LOOP_HPP
