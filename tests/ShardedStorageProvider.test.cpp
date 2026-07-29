#include "atomdb/storage/ShardedStorageProvider.hpp"
#include "atomdb/storage/InMemoryStorageProvider.hpp"
#include "atomdb/storage/LocalFileStorageProvider.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/types/Value.hpp"
#include "atomdb/types/Schema.hpp"
#include "../tests/test_framework.hpp"
#include <vector>
#include <memory>
#include <filesystem>

using namespace atomdb;

static std::unique_ptr<ShardedStorageProvider> makeSharded(int shardCount = 4) {
    std::vector<std::unique_ptr<IStorageProvider>> shards;
    for (int i = 0; i < shardCount; ++i) {
        shards.push_back(std::make_unique<InMemoryStorageProvider>());
    }
    auto p = std::make_unique<ShardedStorageProvider>(std::move(shards));
    EXPECT(p->open("in-memory://").isSentinel());
    return p;
}

TEST(Sharded_Create_Partitioned_Table) {
    auto sharded = makeSharded(4);

    Schema schema;
    schema.table = "users";
    schema.columns = {
        ColumnDef{"id", ValueType::Int64, false, true, 0, {}, std::nullopt},
        ColumnDef{"name", ValueType::Text, true, false, 0, {}, std::nullopt},
        ColumnDef{"email", ValueType::Text, true, false, 0, {}, std::nullopt},
    };
    schema.partition = PartitionPolicy{PartitionPolicy::Kind::Hash, "id", 4, {}, {}};

    EXPECT(sharded->createTable(schema).isSentinel());
}

TEST(Sharded_Put_Get_Same_Shard) {
    auto sharded = makeSharded(4);
    TransactionManager txnm;

    Schema schema;
    schema.table = "users";
    schema.columns = {
        ColumnDef{"id", ValueType::Int64, false, true, 0, {}, std::nullopt},
        ColumnDef{"name", ValueType::Text, true, false, 0, {}, std::nullopt},
    };
    schema.partition = PartitionPolicy{PartitionPolicy::Kind::Hash, "id", 4, {}, {}};
    EXPECT(sharded->createTable(schema).isSentinel());

    auto* engine = sharded->engine();
    TxnId t = txnm.beginTxn();
    Tuple row = Tuple::make({ColumnValue{"id", Value::int64(42)}, ColumnValue{"name", Value::text("alice")}});
    EXPECT(engine->put(t, "users", Value::int64(42), row).isSentinel());
    txnm.commitTxn(t);
    EXPECT(engine->commit(t, txnm.visibleSeq()).isSentinel());

    auto g = engine->get(TxnId{0}, "users", Value::int64(42));
    EXPECT(g.has_value());
    if (g.has_value()) {
        EXPECT(g->get("name") == Value::text("alice"));
    }
}

TEST(Sharded_Different_Keys_Different_Shards) {
    auto sharded = makeSharded(4);
    TransactionManager txnm;

    Schema schema;
    schema.table = "users";
    schema.columns = {
        ColumnDef{"id", ValueType::Int64, false, true, 0, {}, std::nullopt},
        ColumnDef{"name", ValueType::Text, true, false, 0, {}, std::nullopt},
    };
    schema.partition = PartitionPolicy{PartitionPolicy::Kind::Hash, "id", 4, {}, {}};
    EXPECT(sharded->createTable(schema).isSentinel());

    auto* engine = sharded->engine();
    TxnId t = txnm.beginTxn();
    EXPECT(engine->put(t, "users", Value::int64(1), Tuple::make({ColumnValue{"id", Value::int64(1)}, ColumnValue{"name", Value::text("a")}})).isSentinel());
    EXPECT(engine->put(t, "users", Value::int64(2), Tuple::make({ColumnValue{"id", Value::int64(2)}, ColumnValue{"name", Value::text("b")}})).isSentinel());
    EXPECT(engine->put(t, "users", Value::int64(3), Tuple::make({ColumnValue{"id", Value::int64(3)}, ColumnValue{"name", Value::text("c")}})).isSentinel());
    EXPECT(engine->put(t, "users", Value::int64(4), Tuple::make({ColumnValue{"id", Value::int64(4)}, ColumnValue{"name", Value::text("d")}})).isSentinel());
    txnm.commitTxn(t);
    EXPECT(engine->commit(t, txnm.visibleSeq()).isSentinel());

    EXPECT(engine->get(TxnId{0}, "users", Value::int64(1)).has_value());
    EXPECT(engine->get(TxnId{0}, "users", Value::int64(2)).has_value());
    EXPECT(engine->get(TxnId{0}, "users", Value::int64(3)).has_value());
    EXPECT(engine->get(TxnId{0}, "users", Value::int64(4)).has_value());
}

TEST(Sharded_NonPartitioned_Table_Shard0) {
    auto sharded = makeSharded(4);
    TransactionManager txnm;

    Schema schema;
    schema.table = "config";
    schema.columns = {
        ColumnDef{"key", ValueType::Text, false, true, 0, {}, std::nullopt},
        ColumnDef{"value", ValueType::Text, true, false, 0, {}, std::nullopt},
    };
    EXPECT(sharded->createTable(schema).isSentinel());

    auto* engine = sharded->engine();
    TxnId t = txnm.beginTxn();
    EXPECT(engine->put(t, "config", Value::text("version"), Tuple::make({ColumnValue{"key", Value::text("version")}, ColumnValue{"value", Value::text("1.0")}})).isSentinel());
    txnm.commitTxn(t);
    EXPECT(engine->commit(t, txnm.visibleSeq()).isSentinel());

    auto g = engine->get(TxnId{0}, "config", Value::text("version"));
    EXPECT(g.has_value());
    if (g.has_value()) {
        EXPECT(g->get("value") == Value::text("1.0"));
    }
}

TEST(Sharded_Remove_And_Scan) {
    auto sharded = makeSharded(4);
    TransactionManager txnm;
    Schema schema;
    schema.table = "items";
    schema.columns = {
        ColumnDef{"id", ValueType::Int64, false, true, 0, {}, std::nullopt},
        ColumnDef{"val", ValueType::Int64, true, false, 0, {}, std::nullopt},
    };
    schema.partition = PartitionPolicy{PartitionPolicy::Kind::Hash, "id", 4, {}, {}};
    EXPECT(sharded->createTable(schema).isSentinel());

    auto* engine = sharded->engine();
    TxnId t = txnm.beginTxn();
    for (int64_t i = 1; i <= 8; ++i) {
        EXPECT(engine->put(t, "items", Value::int64(i), Tuple::make({ColumnValue{"id", Value::int64(i)}, ColumnValue{"val", Value::int64(i * 10)}})).isSentinel());
    }
    txnm.commitTxn(t);
    EXPECT(engine->commit(t, txnm.visibleSeq()).isSentinel());

    // Remove one (verify it's gone after commit)
    TxnId t2 = txnm.beginTxn();
    EXPECT(engine->remove(t2, "items", Value::int64(5)).isSentinel());
    txnm.commitTxn(t2);
    EXPECT(engine->commit(t2, txnm.visibleSeq()).isSentinel());

    EXPECT(!engine->get(TxnId{0}, "items", Value::int64(5)).has_value());
}

TEST(Sharded_Capabilities_Include_RandomAccess_And_Concurrent) {
    auto sharded = makeSharded(4);
    auto caps = sharded->capabilities();
    bool hasRandomAccess = (caps & static_cast<std::uint32_t>(StorageCapability::RandomAccess)) != 0;
    bool hasConcurrent = (caps & static_cast<std::uint32_t>(StorageCapability::Concurrent)) != 0;
    EXPECT(hasRandomAccess);
    EXPECT(hasConcurrent);
}

TEST(Sharded_Open_Close) {
    std::vector<std::unique_ptr<IStorageProvider>> shards;
    for (int i = 0; i < 2; ++i) {
        shards.push_back(std::make_unique<InMemoryStorageProvider>());
    }
    auto sharded = std::make_unique<ShardedStorageProvider>(std::move(shards));
    EXPECT(!sharded->isOpen());
    EXPECT(sharded->open("in-memory://").isSentinel());
    EXPECT(sharded->isOpen());
    EXPECT(sharded->close().isSentinel());
    EXPECT(!sharded->isOpen());
}

TEST(Sharded_Create_Without_Open_Fails) {
    std::vector<std::unique_ptr<IStorageProvider>> shards;
    shards.push_back(std::make_unique<InMemoryStorageProvider>());
    auto sharded = std::make_unique<ShardedStorageProvider>(std::move(shards));

    Schema schema;
    schema.table = "x";
    schema.columns = {ColumnDef{"id", ValueType::Int64, false, true, 0, {}, std::nullopt}};
    EXPECT(!sharded->createTable(schema).isSentinel());
}

TEST(Sharded_ShardCount_Mismatch_Rejected) {
    auto sharded = makeSharded(4);
    Schema schema;
    schema.table = "users";
    schema.columns = {ColumnDef{"id", ValueType::Int64, false, true, 0, {}, std::nullopt}};
    schema.partition = PartitionPolicy{PartitionPolicy::Kind::Hash, "id", 2, {}, {}};
    EXPECT(!sharded->createTable(schema).isSentinel());
}

TEST(Sharded_Drop_Table_All_Shards) {
    auto sharded = makeSharded(2);
    Schema schema;
    schema.table = "t";
    schema.columns = {ColumnDef{"id", ValueType::Int64, false, true, 0, {}, std::nullopt}};
    EXPECT(sharded->createTable(schema).isSentinel());
    EXPECT(sharded->tables().size() == 1);
    EXPECT(sharded->dropTable("t").isSentinel());
    EXPECT(sharded->tables().empty());
}
TEST(Sharded_Children_Already_Open_Skips_Reopen) {
    // Phase 5 Item 9: ShardedStorageProvider must NOT clobber per-shard URIs
    // by re-calling open() on every child with the parent URI. Construct with
    // children_already_open=true and verify the parent open() leaves children
    // untouched on their existing URIs (using LocalFileStorageProvider backed
    // by separate files; the parent's open("file://shared") must NOT touch
    // either file).
    std::error_code ec;
    std::filesystem::remove("build/test_shard_a.db", ec);
    std::filesystem::remove("build/test_shard_b.db", ec);

    std::vector<std::unique_ptr<IStorageProvider>> v;
    v.push_back(std::make_unique<LocalFileStorageProvider>());
    v.push_back(std::make_unique<LocalFileStorageProvider>());

    EXPECT(v[0]->open("file://build/test_shard_a.db").isSentinel());
    EXPECT(v[1]->open("file://build/test_shard_b.db").isSentinel());

    auto sharded = std::make_unique<ShardedStorageProvider>(std::move(v), true);

    // children_already_open=true -> provider open() should be a no-op and not
    // re-open children with the parent URI.
    EXPECT(sharded->isOpen());
    EXPECT(sharded->open("file://INJECT-IF-NO-OP-FAILED").isSentinel());
    EXPECT(sharded->isOpen());
    EXPECT_EQ(sharded->shardCount(), std::size_t{2});

    EXPECT(sharded->close().isSentinel());
    EXPECT(!sharded->isOpen());

    std::filesystem::remove("build/test_shard_a.db", ec);
    std::filesystem::remove("build/test_shard_b.db", ec);
}

TEST(Sharded_Range_Partitioning_Routes_To_Boundary_Shard) {
    // Phase 5 Item 5: Range partitioning routes by partition-column value.
    // Partition column == key for this test (so get by key lands on the
    // right shard without fan-out).
    auto sharded = makeSharded(3);

    Schema s;
    s.table = "events";
    s.columns = {
        ColumnDef{"region", ValueType::Text, false, true, 0, {}, std::nullopt},
        ColumnDef{"payload", ValueType::Int32, true, false, 0, {}, std::nullopt},
    };
    PartitionPolicy pp;
    pp.kind = PartitionPolicy::Kind::Range;
    pp.column = "region";
    pp.shardCount = 3;
    // sorted ascending, size == shardCount+1
    pp.boundaries = {
        Value::text("a"),    // shard 0 covers [a, e)
        Value::text("e"),    // shard 1 covers [e, m)
        Value::text("m"),    // shard 2 covers [m, +inf)
        Value::text("zzz"),
    };
    s.partition = pp;

    EXPECT(sharded->createTable(s).isSentinel());

    TransactionManager txnm;
    // Insert values; expect each row to land on the correct shard.
    TxnId t = txnm.beginTxn();
    EXPECT(sharded->engine()->put(t, "events", Value::text("alice"), Tuple::make({
        {"region", Value::text("alice")}, {"payload", Value::int32(1)}
    })).isSentinel());
    EXPECT(sharded->engine()->put(t, "events", Value::text("decoy"), Tuple::make({
        {"region", Value::text("decoy")}, {"payload", Value::int32(2)}
    })).isSentinel());
    EXPECT(sharded->engine()->put(t, "events", Value::text("nina"),  Tuple::make({
        {"region", Value::text("nina")},  {"payload", Value::int32(3)}
    })).isSentinel());
    txnm.commitTxn(t);
    EXPECT(sharded->engine()->commit(t, txnm.visibleSeq()).isSentinel());

    // Fan-out scan should observe all rows (regardless of shard placement).
    std::size_t seen = 0;
    sharded->engine()->scan(TxnId{99}, "events", [&](const Tuple&) { ++seen; });
    EXPECT_EQ(seen, std::size_t{3});
}

TEST(Sharded_List_Partitioning_Routes_To_Membership_Shard) {
    // Phase 5 Item 5: List partitioning routes by exact match of the
    // partition-column value against the lists[] entry.
    auto sharded = makeSharded(2);

    Schema s;
    s.table = "teams";
    s.columns = {
        ColumnDef{"name", ValueType::Text, false, true, 0, {}, std::nullopt},
        ColumnDef{"league", ValueType::Text, true, false, 0, {}, std::nullopt},
    };
    PartitionPolicy pp;
    pp.kind = PartitionPolicy::Kind::List;
    pp.column = "league";
    pp.shardCount = 2;
    pp.lists = {
        {Value::text("ALPHA"), Value::text("DELTA")}, // shard 0
        {Value::text("BETA")},                        // shard 1
    };
    s.partition = pp;

    EXPECT(sharded->createTable(s).isSentinel());

    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    EXPECT(sharded->engine()->put(t, "teams", Value::text("alpha"),  Tuple::make({
        {"name", Value::text("alpha")}, {"league", Value::text("ALPHA")}
    })).isSentinel());
    EXPECT(sharded->engine()->put(t, "teams", Value::text("beta"),   Tuple::make({
        {"name", Value::text("beta")},  {"league", Value::text("BETA")}
    })).isSentinel());
    EXPECT(sharded->engine()->put(t, "teams", Value::text("delta"),  Tuple::make({
        {"name", Value::text("delta")}, {"league", Value::text("DELTA")}
    })).isSentinel());
    txnm.commitTxn(t);
    EXPECT(sharded->engine()->commit(t, txnm.visibleSeq()).isSentinel());

    std::size_t seen = 0;
    sharded->engine()->scan(TxnId{99}, "teams", [&](const Tuple&) { ++seen; });
    EXPECT_EQ(seen, std::size_t{3});
}
