#include "test_framework.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "atomdb/contracts/IAccessPlugin.hpp"
#include "atomdb/contracts/IStorageProvider.hpp"
#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/core/EngineDispatcher.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/storage/InMemoryStorageProvider.hpp"
#include "atomdb/types/Command.hpp"
#include "atomdb/types/Result.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/TxnId.hpp"
#include "atomdb/types/Value.hpp"

using namespace atomdb;

// Minimal in-test ISession: yields a single SELECT command, then EOF.
struct OneShotSession : public ISession {
    bool returned = false;
    std::atomic<bool>& doneFlag;
    explicit OneShotSession(std::atomic<bool>& d) : doneFlag(d) {}

    std::optional<Command> nextCommand() override {
        if (returned) return std::nullopt;
        returned = true;
        return Command(CommandType::Select, std::string{"users"});
    }

    void present(const ResultSet& /*rs*/) override {
        doneFlag.store(true);
    }
    void present(const DbError& /*err*/) override {
        doneFlag.store(true);
    }
    void close() override {}
};

TEST(EngineDispatcher_BasicEnqueueAndRun) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    EngineDispatcher dispatcher(2, provider.get(), txnm, lockMgr, dd, std::chrono::seconds(50), std::chrono::milliseconds(0));

    std::atomic<bool> done{false};
    auto session = std::make_unique<OneShotSession>(done);
    dispatcher.enqueue(std::move(session));

    // Wait up to 2 seconds for the worker to run the session.
    for (int i = 0; i < 200 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT(done.load());

    // After the session ends, metrics must reflect exactly one enqueue +
    // one completion, no failures.
    EXPECT_EQ(dispatcher.metrics().sessionsEnqueued.load(), std::uint64_t{1});
    EXPECT_EQ(dispatcher.metrics().sessionsCompleted.load(), std::uint64_t{1});
    EXPECT_EQ(dispatcher.metrics().sessionsFailed.load(), std::uint64_t{0});
    EXPECT(dispatcher.metrics().totalLatencyMicros.load() > 0);

    dispatcher.shutdown();
}

TEST(EngineDispatcher_MetricsSnapshot_IsValidJSON_Shape) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    EngineDispatcher dispatcher(2, provider.get(), txnm, lockMgr, dd, std::chrono::seconds(50), std::chrono::milliseconds(0));

    auto snap = dispatcher.renderMetricsSnapshot();
    EXPECT(snap.find("\"sessions_enqueued\":") != std::string::npos);
    EXPECT(snap.find("\"sessions_completed\":") != std::string::npos);
    EXPECT(snap.find("\"sessions_failed\":") != std::string::npos);
    EXPECT(snap.find("\"total_latency_us\":") != std::string::npos);
    EXPECT(snap.find("\"max_latency_us\":") != std::string::npos);
    EXPECT(snap.find("\"latency_buckets\":[") != std::string::npos);
    EXPECT(snap.find("\"worker_count\":2") != std::string::npos);

    dispatcher.shutdown();
}

TEST(EngineDispatcher_Shutdown_StopsAccepting) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    EngineDispatcher dispatcher(2, provider.get(), txnm, lockMgr, dd, std::chrono::seconds(50), std::chrono::milliseconds(0));
    dispatcher.shutdown();

    // After shutdown, enqueue should not silently accept: any session is
    // presented an error rather than queued.
    std::atomic<bool> done{false};
    auto session = std::make_unique<OneShotSession>(done);
    dispatcher.enqueue(std::move(session));
    // Wait briefly — should be error-presented almost immediately.
    for (int i = 0; i < 50; ++i) {
        if (done.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT(done.load());
    EXPECT_EQ(dispatcher.metrics().sessionsEnqueued.load(), std::uint64_t{0});
    EXPECT_EQ(dispatcher.metrics().sessionsFailed.load(), std::uint64_t{1});
}

TEST(EngineDispatcher_ConcurrentEnqueue_AllSessionsRun) {
    // Phase 5 Item 15: 8 concurrent sessions, all must complete without
    // deadlock. The LockManager's row-level machinery keeps the table-level
    // lock uncontended for SELECTs (Shared) so this finishes quickly.
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    EngineDispatcher dispatcher(4, provider.get(), txnm, lockMgr, dd, std::chrono::seconds(50), std::chrono::milliseconds(0));

    constexpr int kSessions = 8;
    std::vector<std::atomic<bool>> done(kSessions);
    for (int i = 0; i < kSessions; ++i) {
        done[i].store(false);
        dispatcher.enqueue(std::make_unique<OneShotSession>(done[i]));
    }
    EXPECT_EQ(dispatcher.metrics().sessionsEnqueued.load(), std::uint64_t{kSessions});

    // Wait for all sessions to finish.
    for (int i = 0; i < 1000; ++i) {
        bool all = true;
        for (auto& d : done) if (!d.load()) { all = false; break; }
        if (all) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (auto& d : done) EXPECT(d.load());

    EXPECT_EQ(dispatcher.metrics().sessionsCompleted.load(), std::uint64_t{kSessions});
    EXPECT_EQ(dispatcher.metrics().sessionsFailed.load(), std::uint64_t{0});

    dispatcher.shutdown();
}
