#include "test_framework.hpp"

#include "opendb/core/TransactionManager.hpp"
#include "opendb/storage/CachingStorageEngine.hpp"
#include "opendb/storage/InMemoryStorageProvider.hpp"
#include "opendb/types/Schema.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/TxnId.hpp"
#include "opendb/types/Value.hpp"

TEST(Caching_Get_Miss_Hits_Inner_Then_Caches) {
    using namespace opendb;
    InMemoryStorageProvider inner;
    inner.open("in-memory://");
    Schema s; s.table = "users";
    s.columns.push_back({"_id", ValueType::Int64, false, true, 0, {}, std::nullopt});
    s.columns.push_back({"name", ValueType::Text, false, false, 0, {}, std::nullopt});
    EXPECT(inner.createTable(s).isSentinel());

    auto* base = inner.engine();
    CachingStorageEngine cache(base);

    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    EXPECT(base->put(t, "users", Value::int64(7), Tuple::make({
        {"_id", Value::int64(7)},
        {"name", Value::text("nikhil")}
    })).isSentinel());
    txnm.commitTxn(t);
    EXPECT(base->commit(t, txnm.visibleSeq()).isSentinel());

    // Read via cache; should hit inner once, then cache.
    auto g = cache.get(TxnId{0}, "users", Value::int64(7));
    EXPECT(g.has_value());
    EXPECT(g->get("name").asText() == "nikhil");
    EXPECT_EQ(cache.cacheSize(), std::size_t{1});
}

TEST(Caching_Put_Invalidates_Entry) {
    using namespace opendb;
    InMemoryStorageProvider inner;
    inner.open("in-memory://");
    Schema s; s.table = "users";
    s.columns.push_back({"_id", ValueType::Int64, false, true, 0, {}, std::nullopt});
    s.columns.push_back({"name", ValueType::Text, false, false, 0, {}, std::nullopt});
    EXPECT(inner.createTable(s).isSentinel());

    auto* base = inner.engine();
    CachingStorageEngine cache(base);

    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    EXPECT(base->put(t, "users", Value::int64(1), Tuple::make({
        {"_id", Value::int64(1)},
        {"name", Value::text("alice")}
    })).isSentinel());
    txnm.commitTxn(t);
    EXPECT(base->commit(t, txnm.visibleSeq()).isSentinel());

    auto first = cache.get(TxnId{0}, "users", Value::int64(1));
    EXPECT(first.has_value());
    EXPECT(first->get("name").asText() == "alice");
    EXPECT_EQ(cache.cacheSize(), std::size_t{1});

    TxnId t2 = txnm.beginTxn();
    // Rewrite via cache: should invalidate.
    EXPECT(cache.put(t2, "users", Value::int64(1), Tuple::make({
        {"_id", Value::int64(1)},
        {"name", Value::text("bob")}
    })).isSentinel());
    txnm.commitTxn(t2);
    EXPECT(cache.commit(t2, txnm.visibleSeq()).isSentinel());

    auto second = cache.get(TxnId{0}, "users", Value::int64(1));
    EXPECT(second.has_value());
    EXPECT(second->get("name").asText() == "bob");
}

TEST(Caching_Scan_Forwarded_Through_Inner) {
    using namespace opendb;
    InMemoryStorageProvider inner;
    inner.open("in-memory://");
    Schema s; s.table = "users";
    s.columns.push_back({"_id", ValueType::Int64, false, true, 0, {}, std::nullopt});
    s.columns.push_back({"name", ValueType::Text, false, false, 0, {}, std::nullopt});
    EXPECT(inner.createTable(s).isSentinel());

    auto* base = inner.engine();
    CachingStorageEngine cache(base);

    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    for (int i = 1; i <= 5; ++i) {
        EXPECT(base->put(t, "users", Value::int64(i), Tuple::make({
            {"_id", Value::int64(i)},
            {"name", Value::text("u_" + std::to_string(i))}
        })).isSentinel());
    }
    txnm.commitTxn(t);
    EXPECT(base->commit(t, txnm.visibleSeq()).isSentinel());

    std::vector<std::int64_t> ids;
    cache.scan(TxnId{0}, "users", [&](const Tuple& r) {
        ids.push_back(r.get("_id").asInt64());
    });
    EXPECT_EQ(ids.size(), std::size_t{5});
}
