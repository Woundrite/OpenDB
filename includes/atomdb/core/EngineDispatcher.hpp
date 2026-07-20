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
    explicit EngineDispatcher(
        std::size_t workerCount,
        IStorageProvider* storage,
        TransactionManager& txnm,
        LockManager& lockMgr,
        DeadlockDetector& deadlock
    );

    ~EngineDispatcher() override;

    // IEngineDispatcher
    void enqueue(std::unique_ptr<ISession> session) override;

    // Control
    void shutdown();  // graceful: stops accepting, drains queue, joins workers
    bool isRunning() const noexcept;
    std::size_t workerCount() const noexcept;
    std::size_t pendingCount() const;

private:
    // Worker thread main loop
    void workerLoop(std::size_t workerId);

    // Execute a single session to completion
    void runSession(std::unique_ptr<ISession> session);

    // Core components (non-owning)
    IStorageProvider* storage_;
    TransactionManager& txnm_;
    LockManager& lockMgr_;
    DeadlockDetector& deadlock_;

    // Thread pool
    std::vector<std::jthread> workers_;
    std::queue<std::unique_ptr<ISession>> queue_;
    mutable std::mutex queueMu_;
    std::condition_variable queueCv_;
    std::counting_semaphore<10000> queueSem_{0};

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> shuttingDown_{false};
};

} // namespace atomdb

#endif // ATOMDB_ENGINE_DISPATCHER_HPP