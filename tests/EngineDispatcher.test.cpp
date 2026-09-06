#include "test_framework.hpp"

#include <atomic>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "opendb/contracts/IAccessPlugin.hpp"
#include "opendb/contracts/IStorageProvider.hpp"
#include "opendb/core/DeadlockDetector.hpp"
#include "opendb/core/EngineDispatcher.hpp"
#include "opendb/core/LockManager.hpp"
#include "opendb/core/TransactionManager.hpp"
#include "opendb/storage/InMemoryStorageProvider.hpp"
#include "opendb/types/Command.hpp"
#include "opendb/types/Result.hpp"
#include "opendb/types/DbError.hpp"
#include "opendb/types/Schema.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/TxnId.hpp"
#include "opendb/types/Value.hpp"

using namespace opendb;

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

// G.1: an explicit-txn command's table lock must stay held across commands
// (until COMMIT/ROLLBACK), not released after each statement. Probe: while a
// session holds an explicit txn open, a concurrent auto-commit writer on the
// same table must time out; once that session ends without committing, the
// teardown leak-guard must release the lock so the next writer succeeds.
TEST(EngineDispatcher_ExplicitTxn_HoldsLockAcrossCommands) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    Schema users;
    users.table = "users";
    ColumnDef idCol;
    idCol.name = "_id";
    idCol.type = ValueType::Int64;
    idCol.primaryKey = true;
    ColumnDef nameCol;
    nameCol.name = "name";
    nameCol.type = ValueType::Text;
    users.columns = {idCol, nameCol};
    EXPECT(provider->createTable(users).isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    // Short lock wait so the contention probe fails fast instead of hanging.
    EngineDispatcher dispatcher(2, provider.get(), txnm, lockMgr, dd,
                                std::chrono::milliseconds(200),
                                std::chrono::milliseconds(0));

    // Session A: yields one explicit-txn INSERT, then blocks on a latch
    // (simulating a client mid-transaction), then EOF.
    struct ExplicitTxnSession : public ISession {
        enum class Stage { Yield, Hold, Done } stage = Stage::Yield;
        std::atomic<bool>& doneFlag;
        std::latch& hold;
        explicit ExplicitTxnSession(std::atomic<bool>& d, std::latch& h)
            : doneFlag(d), hold(h) {}

        std::optional<Command> nextCommand() override {
            switch (stage) {
                case Stage::Yield: {
                    stage = Stage::Hold;
                    Tuple row;
                    row.set("_id", Value::int64(1));
                    row.set("name", Value::text("a"));
                    return Command(CommandType::Insert, std::string{"users"},
                                   std::nullopt, std::move(row), {},
                                   TxnId{424242});
                }
                case Stage::Hold:
                    hold.wait();  // client "thinks" mid-transaction
                    stage = Stage::Done;
                    return std::nullopt;
                case Stage::Done:
                default:
                    return std::nullopt;
            }
        }
        void present(const ResultSet&) override { doneFlag.store(true); }
        void present(const DbError&) override { doneFlag.store(true); }
        void close() override {}
    };

    std::atomic<bool> aDone{false};
    std::latch hold{1};
    dispatcher.enqueue(std::make_unique<ExplicitTxnSession>(aDone, hold));

    // Wait for session A's INSERT to be processed (lock acquired).
    for (int i = 0; i < 100 && !aDone.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT(aDone.load());  // INSERT dispatched; txn 424242 still open

    // Probe: concurrent auto-commit INSERT on the same table must hit the
    // lock timeout because txn 424242 still holds the Exclusive lock.
    std::atomic<bool> probeDone{false};
    std::atomic<bool> probeTimedOut{false};
    struct ProbeSession : public ISession {
        bool returned = false;
        std::atomic<bool>& doneFlag;
        std::atomic<bool>& timedOut;
        explicit ProbeSession(std::atomic<bool>& d, std::atomic<bool>& t)
            : doneFlag(d), timedOut(t) {}
        std::optional<Command> nextCommand() override {
            if (returned) return std::nullopt;
            returned = true;
            Tuple row;
            row.set("_id", Value::int64(2));
            row.set("name", Value::text("b"));
            return Command(CommandType::Insert, std::string{"users"},
                           std::nullopt, std::move(row));
        }
        void present(const ResultSet&) override { doneFlag.store(true); }
        void present(const DbError& err) override {
            timedOut.store(err.code() == DbErrorCode::LockTimeout);
            doneFlag.store(true);
        }
        void close() override {}
    };
    dispatcher.enqueue(std::make_unique<ProbeSession>(probeDone, probeTimedOut));
    for (int i = 0; i < 200 && !probeDone.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT(probeDone.load());
    EXPECT(probeTimedOut.load());  // G.1: lock was still held

    // End session A without commit (client disconnects mid-txn).
    hold.count_down();
    // Give the worker a moment to observe EOF and run session-A teardown
    // (abort open txns + release their locks).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Leak guard: the lock must be gone now, so a fresh writer succeeds.
    std::atomic<bool> afterDone{false};
    std::atomic<bool> afterFailed{false};
    struct AfterSession : public ISession {
        bool returned = false;
        std::atomic<bool>& doneFlag;
        std::atomic<bool>& failed;
        explicit AfterSession(std::atomic<bool>& d, std::atomic<bool>& f)
            : doneFlag(d), failed(f) {}
        std::optional<Command> nextCommand() override {
            if (returned) return std::nullopt;
            returned = true;
            Tuple row;
            row.set("_id", Value::int64(3));
            row.set("name", Value::text("c"));
            return Command(CommandType::Insert, std::string{"users"},
                           std::nullopt, std::move(row));
        }
        void present(const ResultSet&) override { doneFlag.store(true); }
        void present(const DbError&) override { failed.store(true); doneFlag.store(true); }
        void close() override {}
    };
    dispatcher.enqueue(std::make_unique<AfterSession>(afterDone, afterFailed));
    for (int i = 0; i < 200 && !afterDone.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT(afterDone.load());
    EXPECT(!afterFailed.load());  // lock released by teardown — no leak

    dispatcher.shutdown();
}
