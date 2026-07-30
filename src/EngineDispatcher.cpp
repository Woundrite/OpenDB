#include "atomdb/core/EngineDispatcher.hpp"
#include "atomdb/contracts/IAccessPlugin.hpp"
#include "atomdb/contracts/ICommandSource.hpp"
#include "atomdb/contracts/IStorageProvider.hpp"
#include "atomdb/contracts/IStorageEngine.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/types/Command.hpp"
#include "atomdb/types/Result.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/TxnId.hpp"
#include <chrono>
#include <iostream>
#include <sstream>

namespace atomdb {

EngineDispatcher::EngineDispatcher(
    std::size_t workerCount,
    IStorageProvider* storage,
    TransactionManager& txnm,
    LockManager& lockMgr,
    DeadlockDetector& deadlock
) : storage_(storage), txnm_(txnm), lockMgr_(lockMgr), deadlock_(deadlock) {
    if (workerCount == 0) workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0) workerCount = 1;

    running_.store(true);
    workers_.reserve(workerCount);

    for (std::size_t i = 0; i < workerCount; ++i) {
        workers_.emplace_back([this, i] { workerLoop(i); });
    }
}

EngineDispatcher::~EngineDispatcher() {
    shutdown();
}

void EngineDispatcher::enqueue(std::unique_ptr<ISession> session) {
    if (!running_.load() || shuttingDown_.load()) {
        // Dispatcher not running or shutting down — reject session
        if (session) {
            session->present(DbError::internal("dispatcher not accepting sessions"));
            metrics_.sessionsFailed.fetch_add(1);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lk(queueMu_);
        queue_.push(std::move(session));
    }
    metrics_.sessionsEnqueued.fetch_add(1);
    queueSem_.release();
}

void EngineDispatcher::shutdown() {
    if (!shuttingDown_.exchange(true)) {
        running_.store(false);
        // Wake all workers
        for (std::size_t i = 0; i < workers_.size(); ++i) {
            queueSem_.release();
        }
        // jthreads join automatically on destruction
    }
}

bool EngineDispatcher::isRunning() const noexcept {
    return running_.load();
}

std::size_t EngineDispatcher::workerCount() const noexcept {
    return workers_.size();
}

std::size_t EngineDispatcher::pendingCount() const {
    std::lock_guard<std::mutex> lk(queueMu_);
    return queue_.size();
}

void EngineDispatcher::workerLoop(std::size_t workerId) {
    (void)workerId; // unused for now
    while (running_.load()) {
        // Wait for work
        queueSem_.acquire();

        if (!running_.load()) break;

        std::unique_ptr<ISession> session;
        {
            std::lock_guard<std::mutex> lk(queueMu_);
            if (queue_.empty()) continue;
            session = std::move(queue_.front());
            queue_.pop();
        }

        if (session) {
            runSession(std::move(session));
        }
    }
}

void EngineDispatcher::runSession(std::unique_ptr<ISession> session) {
    // Phase 5 Item 15: track per-session latency from enqueue to close.
    auto t0 = std::chrono::steady_clock::now();
    bool ok = true;

    // Get engine from storage provider
    IStorageEngine* engine = storage_->engine();
    if (!engine) {
        session->present(DbError::internal("no engine available"));
        session->close();
        ok = false;
        metrics_.sessionsFailed.fetch_add(1);
        auto t1 = std::chrono::steady_clock::now();
        auto micros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        recordLatency(micros);
        return;
    }

    // Session loop: pull commands and dispatch via EngineLoop logic
    for (;;) {
        auto optCmd = session->nextCommand();
        if (!optCmd) break; // EOF

        Command cmd = std::move(*optCmd);

        // Reuse or begin transaction
        const bool autoCommit = !cmd.txnId.has_value();
        TxnId txnId = cmd.txnId.value_or(txnm_.beginTxn());

        // Determine lock mode
        using atomdb::LockMode;
        LockMode mode = (cmd.type == CommandType::Select) ? LockMode::Shared : LockMode::Exclusive;

        // Deadlock pre-check
        auto cycleVictim = deadlock_.detectCycle(txnId);
        if (cycleVictim.has_value()) {
            txnm_.abortTxn(*cycleVictim);
            lockMgr_.release(*cycleVictim);
            session->present(DbError::deadlock("cycle detected for " + txnId.toString()));
            continue;
        }

        // Acquire lock (blocks until granted)
        lockMgr_.acquire(txnId, cmd.table, mode);

        // Dispatch
        ResultSet rs;
        std::optional<DbError> errorOpt;

        try {
            switch (cmd.type) {
                case CommandType::Select: {
                    rs.success = true;
                    auto* enginePtr = storage_->engine();
                    enginePtr->scan(txnId, cmd.table, [&](const Tuple& row) {
                        if (!cmd.where || cmd.where->evaluate(row)) {
                            if (cmd.projections.empty() ||
                                (cmd.projections.size() == 1 && cmd.projections[0] == "*")) {
                                rs.rows.push_back(row);
                            } else {
                                Tuple projected;
                                for (const auto& pc : cmd.projections) {
                                    if (pc == "*") continue;
                                    if (auto v = row.maybeGet(pc))
                                        projected.set(pc, std::move(*v));
                                }
                                rs.rows.push_back(std::move(projected));
                            }
                        }
                    });
                    break;
                }
                case CommandType::Insert: {
                    Value key = Value::null();
                    if (cmd.values) {
                        auto idOpt = cmd.values->maybeGet("_id");
                        if (idOpt && !idOpt->isNull()) key = *idOpt;
                    }
                    auto err = storage_->engine()->put(txnId, cmd.table, key, *cmd.values);
                    if (!err.isSentinel()) errorOpt = err;
                    else rs = ResultSet(true, {});
                    break;
                }
                case CommandType::Update: {
                    if (!cmd.where) { errorOpt = DbError::internal("Update requires WHERE"); break; }
                    if (!cmd.values) { errorOpt = DbError::internal("Update requires values"); break; }

                    std::vector<Value> keysToUpdate;
                    storage_->engine()->scan(txnId, cmd.table, [&](const Tuple& row) {
                        if (cmd.where->evaluate(row)) {
                            auto pkOpt = row.maybeGet("_id");
                            if (pkOpt) keysToUpdate.push_back(*pkOpt);
                        }
                    });
                    bool any = false;
                    for (const auto& k : keysToUpdate) {
                        Tuple row = *cmd.values;
                        row.set("_id", k);
                        auto err = storage_->engine()->put(txnId, cmd.table, k, row);
                        if (!err.isSentinel()) { errorOpt = err; break; }
                        any = true;
                    }
                    if (!errorOpt && !any) errorOpt = DbError::notFound("no matching rows to update");
                    else if (!errorOpt) rs = ResultSet(true, {});
                    break;
                }
                case CommandType::Delete: {
                    if (!cmd.where) { errorOpt = DbError::internal("Delete requires WHERE"); break; }
                    std::vector<Value> keysToDelete;
                    storage_->engine()->scan(txnId, cmd.table, [&](const Tuple& row) {
                        if (cmd.where->evaluate(row)) {
                            auto pkOpt = row.maybeGet("_id");
                            if (pkOpt) keysToDelete.push_back(*pkOpt);
                        }
                    });
                    bool any = false;
                    for (const auto& k : keysToDelete) {
                        auto err = storage_->engine()->remove(txnId, cmd.table, k);
                        if (!err.isSentinel()) { errorOpt = err; break; }
                        any = true;
                    }
                    if (!errorOpt && !any) errorOpt = DbError::notFound("no matching rows to delete");
                    else if (!errorOpt) rs = ResultSet(true, {});
                    break;
                }
            }
        } catch (const std::exception& e) {
            errorOpt = DbError::internal(std::string("dispatch exception: ") + e.what());
        }

        // Present result or error
        if (errorOpt) {
            session->present(*errorOpt);
        } else {
            session->present(rs);
        }

        // Auto-commit
        if (autoCommit) {
            txnm_.commitTxn(txnId);
            auto commitErr = storage_->engine()->commit(txnId, txnm_.visibleSeq());
            if (!commitErr.isSentinel()) {
                session->present(commitErr);
            }
        }
        lockMgr_.release(txnId);
    }

    // Close session transport
    session->close();

    // Phase 5 Item 15: finalize metrics.
    auto t1 = std::chrono::steady_clock::now();
    auto micros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    if (ok) {
        metrics_.sessionsCompleted.fetch_add(1);
    } else {
        metrics_.sessionsFailed.fetch_add(1);
    }
    recordLatency(micros);
}

void EngineDispatcher::recordLatency(std::uint64_t micros) {
    metrics_.totalLatencyMicros.fetch_add(micros);
    // Update max via CAS loop (cheaper than taking a mutex).
    std::uint64_t cur = metrics_.maxLatencyMicros.load();
    while (micros > cur) {
        if (metrics_.maxLatencyMicros.compare_exchange_weak(cur, micros)) break;
    }
    // Bucket into 2^i bands. Bucket index = floor(log2(micros)) capped at kLatencyBuckets-1.
    std::size_t idx = 0;
    std::uint64_t v = micros;
    while (v > 1 && idx + 1 < Metrics::kLatencyBuckets) { v >>= 1; ++idx; }
    if (idx >= Metrics::kLatencyBuckets) idx = Metrics::kLatencyBuckets - 1;
    metrics_.latencyBuckets[idx].fetch_add(1);
}

std::string EngineDispatcher::renderMetricsSnapshot() const {
    std::ostringstream os;
    os << "{"
       << "\"sessions_enqueued\":" << metrics_.sessionsEnqueued.load()
       << ",\"sessions_completed\":" << metrics_.sessionsCompleted.load()
       << ",\"sessions_failed\":" << metrics_.sessionsFailed.load()
       << ",\"total_latency_us\":" << metrics_.totalLatencyMicros.load()
       << ",\"max_latency_us\":" << metrics_.maxLatencyMicros.load()
       << ",\"latency_buckets\":[";
    for (std::size_t i = 0; i < Metrics::kLatencyBuckets; ++i) {
        if (i) os << ",";
        os << metrics_.latencyBuckets[i].load();
    }
    os << "]"
       << ",\"worker_count\":" << workers_.size()
       << ",\"pending_count\":" << pendingCount()
       << "}";
    return os.str();
}

} // namespace atomdb