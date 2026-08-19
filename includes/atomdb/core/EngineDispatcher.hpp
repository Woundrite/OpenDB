#ifndef ATOMDB_ENGINE_DISPATCHER_HPP
#define ATOMDB_ENGINE_DISPATCHER_HPP

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <semaphore>
#include <thread>
#include <vector>

#include "atomdb/contracts/IAccessPlugin.hpp"
#include "atomdb/contracts/ICommandSource.hpp"

namespace atomdb {

// Forward declarations
class IStorageProvider;
class IStorageEngine;
class TransactionManager;
class LockManager;
class DeadlockDetector;

// EngineDispatcher: thread-pool dispatcher that runs ISessions on worker threads.
// Uses C++20 std::jthread, std::counting_semaphore, std::latch.
class EngineDispatcher : public IEngineDispatcher {
public:
    // workerCount: number of worker threads (default: hardware_concurrency)
    // core: pointers to core engine components needed by workers
    // lockTimeout: maximum time a transaction waits for a lock (default 50s);
    //              if exceeded, the transaction is aborted with DbError::lockTimeout.
    // queryTimeout: maximum cumulative execution time for a session (default 0 = no timeout);
    //               if exceeded, the transaction is aborted with DbError::queryTimeout.
    explicit EngineDispatcher(
        std::size_t workerCount,
        IStorageProvider* storage,
        TransactionManager& txnm,
        LockManager& lockMgr,
        DeadlockDetector& deadlock,
        std::chrono::milliseconds lockTimeout = std::chrono::seconds(50),
        std::chrono::milliseconds queryTimeout = std::chrono::milliseconds(0)
    );

    ~EngineDispatcher() override;

    // IEngineDispatcher
    void enqueue(std::unique_ptr<ISession> session) override;

    // Control
    void shutdown();  // graceful: stops accepting, drains queue, joins workers
    bool isRunning() const noexcept;
    std::size_t workerCount() const noexcept;
    std::size_t pendingCount() const;

    // Phase 5 Item 15: operational metrics. Atomic counters updated by
    // workers; readable by any thread (the HttpServer's /metrics endpoint
    // renders a snapshot). Cheap enough to update on every session.
    struct Metrics {
        std::atomic<std::uint64_t> sessionsEnqueued{0};
        std::atomic<std::uint64_t> sessionsCompleted{0};
        std::atomic<std::uint64_t> sessionsFailed{0};
        std::atomic<std::uint64_t> totalLatencyMicros{0};
        std::atomic<std::uint64_t> maxLatencyMicros{0};
        // ponytail: a tiny latency histogram. We bucket every measured
        // session into a power-of-two band: <1us, <2us, <4us, ... <16ms,
        // and a saturating ">=16ms" bucket. 18 buckets is plenty for
        // operator-facing dashboards.
        static constexpr std::size_t kLatencyBuckets = 18;
        std::atomic<std::uint64_t> latencyBuckets[kLatencyBuckets]{};

        Metrics() {
            for (auto& b : latencyBuckets) b.store(0);
        }
        Metrics(const Metrics&) = delete;
        Metrics& operator=(const Metrics&) = delete;
    };
    const Metrics& metrics() const noexcept { return metrics_; }

    // Phase 5 Item 15: render a JSON snapshot of the metrics for /metrics.
    std::string renderMetricsSnapshot() const override;

private:
    // Worker thread main loop
    void workerLoop(std::size_t workerId);

    // Execute a single session to completion
    void runSession(std::unique_ptr<ISession> session);

    // Check if query timeout has been exceeded.
    // Returns true if timeout exceeded and session was notified.
    bool checkQueryTimeout(ISession* session,
                           const std::chrono::steady_clock::time_point& startTime);

    // Phase 5 Item 15: record one session's latency into the histogram.
    void recordLatency(std::uint64_t micros);

    // Core components (non-owning)
    IStorageProvider* storage_;
    TransactionManager& txnm_;
    LockManager& lockMgr_;
    DeadlockDetector& deadlock_;
    std::chrono::milliseconds lockTimeout_;
    std::chrono::milliseconds queryTimeout_;

    // Thread pool
    std::vector<std::jthread> workers_;
    std::queue<std::unique_ptr<ISession>> queue_;
    mutable std::mutex queueMu_;
    std::condition_variable queueCv_;
    std::counting_semaphore<10000> queueSem_{0};

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> shuttingDown_{false};

    Metrics metrics_;
};

} // namespace atomdb

#endif // ATOMDB_ENGINE_DISPATCHER_HPP