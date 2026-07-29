#include "test_framework.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/storage/InMemoryStorageProvider.hpp"
#include "atomdb/types/Schema.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/TxnId.hpp"
#include "atomdb/types/Value.hpp"

using namespace atomdb;

namespace {
Schema makeUsersSchema() {
    Schema s;
    s.table = "users";
    s.columns = {
        ColumnDef{"_id", ValueType::Int64, false, true, 0, {}, std::nullopt},
        ColumnDef{"name", ValueType::Text, false, false, 0, {}, std::nullopt},
    };
    return s;
}
} // namespace

TEST(Stress_Concurrent_Insert_NoDataRace) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;

    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());
    EXPECT(provider->createTable(makeUsersSchema()).isSentinel());

    auto engine = provider->engine();
    std::atomic<int> ready{0};

    auto worker = [&](int threadIdx) {
        ready.fetch_add(1);
        // Spin barrier: wait until everyone is ready
        while (ready.load() < kThreads) {
            std::this_thread::yield();
        }

        TransactionManager txnm;
        TxnId txn = txnm.beginTxn();
        for (int i = 0; i < kPerThread; ++i) {
            std::int64_t id = std::int64_t(threadIdx) * 1000 + i;
            Tuple row = Tuple::make({
                {"_id", Value::int64(id)},
                {"name", Value::text("user-" + std::to_string(id))},
            });
            EXPECT(engine->put(txn, "users", Value::int64(id), row).isSentinel());
        }
        txnm.commitTxn(txn);
        EXPECT(engine->commit(txn, txnm.visibleSeq()).isSentinel());
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) t.join();

    // After all threads commit, fan-out scan must see at least kThreads * kPerThread rows.
    std::size_t observed = 0;
    engine->scan(TxnId{99}, "users", [&](const Tuple&) { ++observed; });
    EXPECT_EQ(observed, static_cast<std::size_t>(kThreads * kPerThread));
}

TEST(Stress_Concurrent_Remove_Visible_After_Commit) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());
    EXPECT(provider->createTable(makeUsersSchema()).isSentinel());

    auto engine = provider->engine();

    TransactionManager txnm;
    // Pre-populate 100 rows.
    TxnId seedTxn = txnm.beginTxn();
    for (int i = 1; i <= 100; ++i) {
        Tuple row = Tuple::make({
            {"_id", Value::int64(i)},
            {"name", Value::text("u" + std::to_string(i))},
        });
        EXPECT(engine->put(seedTxn, "users", Value::int64(i), row).isSentinel());
    }
    txnm.commitTxn(seedTxn);
    EXPECT(engine->commit(seedTxn, txnm.visibleSeq()).isSentinel());

    constexpr int kThreads = 4;
    constexpr int kPerThread = 20;
    std::atomic<int> ready{0};
    std::atomic<int> removed{0};

    auto worker = [&](int threadId) {
        ready.fetch_add(1);
        while (ready.load() < kThreads) std::this_thread::yield();

        TransactionManager local;
        TxnId txn = local.beginTxn();
        for (int i = 0; i < kPerThread; ++i) {
            std::int64_t id = threadId * kPerThread + i + 1;
            if (engine->remove(txn, "users", Value::int64(id)).isSentinel()) {
                removed.fetch_add(1);
            }
        }
        local.commitTxn(txn);
        EXPECT(engine->commit(txn, local.visibleSeq()).isSentinel());
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    EXPECT_EQ(removed.load(), kThreads * kPerThread);

    // After commits, fan-out scan must observe 100 - removed rows.
    std::size_t observed = 0;
    engine->scan(TxnId{99}, "users", [&](const Tuple&) { ++observed; });
    EXPECT_EQ(observed, static_cast<std::size_t>(100 - kThreads * kPerThread));
}

TEST(Stress_Concurrent_Reads_On_Stable_Snapshot) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());
    EXPECT(provider->createTable(makeUsersSchema()).isSentinel());

    auto engine = provider->engine();

    // Seed 50 rows under one committed txn.
    TransactionManager txnm;
    TxnId seedTxn = txnm.beginTxn();
    for (int i = 1; i <= 50; ++i) {
        Tuple row = Tuple::make({
            {"_id", Value::int64(i)},
            {"name", Value::text("u" + std::to_string(i))},
        });
        EXPECT(engine->put(seedTxn, "users", Value::int64(i), row).isSentinel());
    }
    txnm.commitTxn(seedTxn);
    EXPECT(engine->commit(seedTxn, txnm.visibleSeq()).isSentinel());

    constexpr int kThreads = 6;
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 100; ++i) {
                std::size_t observed = 0;
                engine->scan(TxnId{99}, "users", [&](const Tuple&) { ++observed; });
                if (observed != 50) errors.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(errors.load(), 0);
}
