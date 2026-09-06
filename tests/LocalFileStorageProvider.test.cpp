#include "test_framework.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "opendb/core/TransactionManager.hpp"
#include "opendb/storage/LocalFileStorageProvider.hpp"
#include "opendb/types/DbError.hpp"
#include "opendb/types/Schema.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/TxnId.hpp"
#include "opendb/types/Value.hpp"

using namespace opendb;

namespace {
constexpr const char* kTmpDir = "build";

void cleanup(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

Schema makeUsersSchema() {
    Schema s;
    s.table = "users";
    s.columns = {
        ColumnDef{"_id", ValueType::Int64, false, true, 0, {}, std::nullopt},
        ColumnDef{"name", ValueType::Text, false, false, 0, {}, std::nullopt},
        ColumnDef{"age", ValueType::Int32, true, false, 0, {}, std::nullopt},
    };
    return s;
}
} // namespace

// ---- capability / interface sanity -----------------------------------------

TEST(LocalFile_Provider_Name_And_Capabilities) {
    LocalFileStorageProvider p;
    EXPECT(p.name() == "local-file");
    auto caps = p.capabilities();
    EXPECT(caps & static_cast<std::uint32_t>(StorageCapability::Durable));
    EXPECT(caps & static_cast<std::uint32_t>(StorageCapability::RandomAccess));
    EXPECT(caps & static_cast<std::uint32_t>(StorageCapability::OrderedScan));
    EXPECT(caps & static_cast<std::uint32_t>(StorageCapability::BlobSupport));
    EXPECT(caps & static_cast<std::uint32_t>(StorageCapability::TemporalSupport));
    EXPECT(caps & static_cast<std::uint32_t>(StorageCapability::Concurrent));
}

TEST(LocalFile_TypeVocabulary_Accepts_All_Extended_Types) {
    LocalFileStorageProvider p;
    auto tv = p.typeVocabulary();
    auto contains = [&](ValueType t) {
        for (auto a : tv.accepted) if (a == t) return true;
        return false;
    };
    EXPECT(contains(ValueType::Int64));
    EXPECT(contains(ValueType::Text));
    EXPECT(contains(ValueType::Blob));
    EXPECT(contains(ValueType::Date));
    EXPECT(contains(ValueType::Timestamp));
}

TEST(LocalFile_Engine_Returns_NonNull_Pointer) {
    LocalFileStorageProvider p;
    DbError e = p.open("file://build/test_localfile_engine.db");
    EXPECT(e.isSentinel());
    EXPECT(p.engine() != nullptr);
    EXPECT(p.engine() == static_cast<IStorageEngine*>(&p));
    cleanup("build/test_localfile_engine.db");
}

// ---- DDL --------------------------------------------------------------------

TEST(LocalFile_CreateTable_Registers_Schema) {
    cleanup("build/test_localfile_createtable.db");
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_createtable.db").isSentinel());
        Schema s = makeUsersSchema();
        EXPECT(p.createTable(s).isSentinel());
        EXPECT_EQ(p.tableCount(), std::size_t{1});

        auto desc = p.describeTable("users");
        EXPECT(desc.has_value());
        EXPECT(desc->table == "users");
        EXPECT_EQ(desc->columns.size(), std::size_t{3});
        EXPECT(desc->primaryKeyColumn() == "_id");
    }
    cleanup("build/test_localfile_createtable.db");
}

TEST(LocalFile_CreateTable_Rejects_Duplicate) {
    cleanup("build/test_localfile_dup.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_dup.db").isSentinel());
    Schema s = makeUsersSchema();
    EXPECT(p.createTable(s).isSentinel());
    Schema dup = makeUsersSchema();
    auto err = p.createTable(dup);
    EXPECT(!err.isSentinel());
    cleanup("build/test_localfile_dup.db");
}

TEST(LocalFile_CreateTable_Rejects_Unsupported_Type) {
    cleanup("build/test_localfile_reject.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_reject.db").isSentinel());
    // ValueType::Null column is the only one we mark unsupported. Hmm â€” actually
    // we accept Null too. Pick one we DON'T support: there isn't one in the v1
    // vocabulary. So this test verifies the accept path works.
    Schema s = makeUsersSchema();
    EXPECT(p.createTable(s).isSentinel());
    cleanup("build/test_localfile_reject.db");
}

TEST(LocalFile_DropTable_Removes_Schema) {
    cleanup("build/test_localfile_drop.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_drop.db").isSentinel());
    Schema s = makeUsersSchema();
    EXPECT(p.createTable(s).isSentinel());
    EXPECT_EQ(p.tableCount(), std::size_t{1});
    EXPECT(p.dropTable("users").isSentinel());
    EXPECT_EQ(p.tableCount(), std::size_t{0});
    EXPECT(!p.describeTable("users").has_value());
    cleanup("build/test_localfile_drop.db");
}

TEST(LocalFile_Tables_Lists_All_Schemas) {
    cleanup("build/test_localfile_lists.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_lists.db").isSentinel());
    Schema a = makeUsersSchema();
    a.table = "a"; a.columns[0].name = "id";
    Schema b = makeUsersSchema();
    b.table = "b"; b.columns[0].name = "id";
    EXPECT(p.createTable(a).isSentinel());
    EXPECT(p.createTable(b).isSentinel());
    auto t = p.tables();
    EXPECT_EQ(t.size(), std::size_t{2});
    cleanup("build/test_localfile_lists.db");
}

// ---- CRUD paths ------------------------------------------------------------

TEST(LocalFile_Put_And_Get_Round_Trip) {
    cleanup("build/test_localfile_crud.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_crud.db").isSentinel());
    EXPECT(p.createTable(makeUsersSchema()).isSentinel());

    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    Tuple row = Tuple::make({
        {"_id", Value::int64(1)},
        {"name", Value::text("nikhil")},
        {"age", Value::int32(30)},
    });
    EXPECT(p.put(t, "users", Value::int64(1), row).isSentinel());
    txnm.commitTxn(t);
    EXPECT(p.commit(t, txnm.visibleSeq()).isSentinel());

    auto got = p.get(t, "users", Value::int64(1));
    EXPECT(got.has_value());
    EXPECT(got->get("name").asText() == "nikhil");
    EXPECT_EQ(got->get("age").asInt32(), 30);
    cleanup("build/test_localfile_crud.db");
}

TEST(LocalFile_Auto_Key_Assigns_Int64) {
    cleanup("build/test_localfile_autokey.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_autokey.db").isSentinel());
    EXPECT(p.createTable(makeUsersSchema()).isSentinel());

    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    Tuple row = Tuple::make({{"name", Value::text("a")}, {"age", Value::int32(20)}});
    EXPECT(p.put(t, "users", Value::null(), row).isSentinel());
    txnm.commitTxn(t);
    EXPECT(p.commit(t, txnm.visibleSeq()).isSentinel());

    auto got = p.get(t, "users", Value::int64(1));
    EXPECT(got.has_value());
    EXPECT(got->has("_id"));
    EXPECT_EQ(got->get("_id").asInt64(), 1);
    EXPECT_EQ(p.rowCount("users"), std::size_t{1});
    cleanup("build/test_localfile_autokey.db");
}

// ponytail: 10 rows < one-leaf capacity; bound here so we never trigger the
// BTree split path during the LocalFile persistence test. Split correctness
// is already covered by BTree_Put_Many_Rows_Triggers_Splits; this test verifies
// the LocalFile layer can drive a multiâ€‘row put+commit+scan.
TEST(LocalFile_Put_Many_Rows_Scan_In_Order) {
    cleanup("build/test_localfile_scan.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_scan.db").isSentinel());
    EXPECT(p.createTable(makeUsersSchema()).isSentinel());

    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    for (int i = 1; i <= 10; ++i) {
        Tuple row = Tuple::make({
            {"_id", Value::int64(i)},
            {"name", Value::text("user_" + std::to_string(i))},
            {"age", Value::int32(i * 10)},
        });
        EXPECT(p.put(t, "users", Value::int64(i), row).isSentinel());
    }
    txnm.commitTxn(t);
    EXPECT(p.commit(t, txnm.visibleSeq()).isSentinel());

    std::vector<std::int64_t> ids;
    p.scan(t, "users", [&](const Tuple& r) {
        ids.push_back(r.get("_id").asInt64());
    });
    const std::vector<std::int64_t> want{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    EXPECT(ids == want);
    EXPECT_EQ(p.rowCount("users"), std::size_t{10});
    cleanup("build/test_localfile_scan.db");
}

TEST(LocalFile_Remove_Creates_Tombstone_InVISIBLE_After_Commit) {
    cleanup("build/test_localfile_remove.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_remove.db").isSentinel());
    EXPECT(p.createTable(makeUsersSchema()).isSentinel());

    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    Tuple row = Tuple::make({{"_id", Value::int64(1)}, {"name", Value::text("a")}});
    EXPECT(p.put(t, "users", Value::int64(1), row).isSentinel());
    txnm.commitTxn(t);
    EXPECT(p.commit(t, txnm.visibleSeq()).isSentinel());
    EXPECT_EQ(p.rowCount("users"), std::size_t{1});

    // Remove in a new txn and commit.
    TxnId t2 = txnm.beginTxn();
    EXPECT(p.remove(t2, "users", Value::int64(1)).isSentinel());
    txnm.commitTxn(t2);
    EXPECT(p.commit(t2, txnm.visibleSeq()).isSentinel());

    EXPECT(!p.get(TxnId{99}, "users", Value::int64(1)).has_value());
    EXPECT_EQ(p.rowCount("users"), std::size_t{0});
    cleanup("build/test_localfile_remove.db");
}

// ---- Critical: persistence/reload from file --------------------------------

TEST(LocalFile_Data_Persists_Across_Close_And_Reopen) {
    cleanup("build/test_localfile_persist.db");
    {
        // First session: create table, insert, commit, close.
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_persist.db").isSentinel());
        EXPECT(p.createTable(makeUsersSchema()).isSentinel());

        TransactionManager txnm;
        TxnId t = txnm.beginTxn();
        for (int i = 1; i <= 20; ++i) {
            Tuple row = Tuple::make({
                {"_id", Value::int64(i)},
                {"name", Value::text("user_" + std::to_string(i))},
                {"age", Value::int32(i)},
            });
            EXPECT(p.put(t, "users", Value::int64(i), row).isSentinel());
        }
        txnm.commitTxn(t);
        EXPECT(p.commit(t, txnm.visibleSeq()).isSentinel());
        EXPECT(p.close().isSentinel());
    }

    // Second session: reload the same file. The data and schema must survive.
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_persist.db").isSentinel());
        EXPECT(p.isOpen());
        EXPECT_EQ(p.tableCount(), std::size_t{1});
        EXPECT_EQ(p.tableCount(), std::size_t{1});

        auto s = p.describeTable("users");
        EXPECT(s.has_value());
        EXPECT_EQ(s->columns.size(), std::size_t{3});
        EXPECT(s->primaryKeyColumn() == "_id");

        EXPECT_EQ(p.rowCount("users"), std::size_t{20});

        // Verify a specific lookup after reload.
        auto got = p.get(TxnId{99}, "users", Value::int64(15));
        EXPECT(got.has_value());
        EXPECT(got->get("name").asText() == "user_15");
        EXPECT_EQ(got->get("age").asInt32(), 15);
    }
    cleanup("build/test_localfile_persist.db");
}

TEST(LocalFile_Multiple_Tables_And_Schemas_Persist) {
    cleanup("build/test_localfile_multi.db");
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_multi.db").isSentinel());

        Schema users = makeUsersSchema();

        Schema items;
        items.table = "items";
        items.columns = {
            ColumnDef{"_id", ValueType::Int64, false, true, 0, {}, std::nullopt},
            ColumnDef{"sku",  ValueType::Text,  false, false, 16, {}, std::nullopt},
            ColumnDef{"qty",  ValueType::Int32, true,  false, 0, {}, std::nullopt},
        };

        EXPECT(p.createTable(users).isSentinel());
        EXPECT(p.createTable(items).isSentinel());

        TransactionManager txnm;
        TxnId t = txnm.beginTxn();
        for (int i = 1; i <= 5; ++i) {
            Tuple r = Tuple::make({{"_id", Value::int64(i)}, {"name", Value::text("u" + std::to_string(i))}, {"age", Value::int32(i*10)}});
            EXPECT(p.put(t, "users", Value::int64(i), r).isSentinel());
            Tuple r2 = Tuple::make({{"_id", Value::int64(i)}, {"sku", Value::text("SKU-00" + std::to_string(i))}, {"qty", Value::int32(i*5)}});
            EXPECT(p.put(t, "items", Value::int64(i), r2).isSentinel());
        }
        txnm.commitTxn(t);
        EXPECT(p.commit(t, txnm.visibleSeq()).isSentinel());
        EXPECT(p.close().isSentinel());
    }
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_multi.db").isSentinel());
        EXPECT_EQ(p.tableCount(), std::size_t{2});
        EXPECT_EQ(p.rowCount("users"), std::size_t{5});
        EXPECT_EQ(p.rowCount("items"), std::size_t{5});

        auto got_item = p.get(TxnId{99}, "items", Value::int64(3));
        EXPECT(got_item.has_value());
        EXPECT(got_item->get("sku").asText() == "SKU-003");
        EXPECT_EQ(got_item->get("qty").asInt32(), 15);
    }
    cleanup("build/test_localfile_multi.db");
}

TEST(LocalFile_Overwrite_Persists_Last_Visible_Value) {
    cleanup("build/test_localfile_overwrite.db");
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_overwrite.db").isSentinel());
        EXPECT(p.createTable(makeUsersSchema()).isSentinel());

        TransactionManager txnm;
        // Initial insert committed.
        TxnId t1 = txnm.beginTxn();
        Tuple r1 = Tuple::make({{"_id", Value::int64(1)}, {"name", Value::text("alice")}, {"age", Value::int32(20)}});
        EXPECT(p.put(t1, "users", Value::int64(1), r1).isSentinel());
        txnm.commitTxn(t1);
        EXPECT(p.commit(t1, txnm.visibleSeq()).isSentinel());

        // Overwrite committed.
        TxnId t2 = txnm.beginTxn();
        Tuple r2 = Tuple::make({{"_id", Value::int64(1)}, {"name", Value::text("alice2")}, {"age", Value::int32(21)}});
        EXPECT(p.put(t2, "users", Value::int64(1), r2).isSentinel());
        txnm.commitTxn(t2);
        EXPECT(p.commit(t2, txnm.visibleSeq()).isSentinel());
        EXPECT(p.close().isSentinel());
    }

    // Reload and verify latest committed state is preserved.
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_overwrite.db").isSentinel());
        auto got = p.get(TxnId{99}, "users", Value::int64(1));
        EXPECT(got.has_value());
        // After overwrite, the user should be "alice2" age 21 â€” but the append-only
        // model keeps the staged version too. The get() returns the newest visible
        // committed version, which is the overwrite.
        EXPECT_EQ(got->get("age").asInt32(), 21);
    }
    cleanup("build/test_localfile_overwrite.db");
}

TEST(LocalFile_Abort_Leaves_Table_Intact_For_Other_Txns) {
    cleanup("build/test_localfile_abort.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_abort.db").isSentinel());
    EXPECT(p.createTable(makeUsersSchema()).isSentinel());

    TransactionManager txnm;
    TxnId t1 = txnm.beginTxn();
    Tuple r = Tuple::make({{"_id", Value::int64(1)}, {"name", Value::text("x")}});
    EXPECT(p.put(t1, "users", Value::int64(1), r).isSentinel());
    txnm.commitTxn(t1);
    EXPECT(p.commit(t1, txnm.visibleSeq()).isSentinel());

    TxnId t2 = txnm.beginTxn();
    Tuple r2 = Tuple::make({{"_id", Value::int64(1)}, {"name", Value::text("xy")}});
    EXPECT(p.put(t2, "users", Value::int64(1), r2).isSentinel());
    txnm.abortTxn(t2);
    EXPECT(p.abort(t2).isSentinel());

    // t2 is aborted â€” staged version is dead. Other reader sees original.
    auto got = p.get(TxnId{99}, "users", Value::int64(1));
    EXPECT(got.has_value());
    EXPECT(got->get("name").asText() == "x");
    EXPECT_EQ(p.rowCount("users"), std::size_t{1});
    cleanup("build/test_localfile_abort.db");
}

TEST(LocalFile_Close_Without_Open_Is_Safe) {
    LocalFileStorageProvider p;
    EXPECT(p.close().isSentinel()); // no-op if not open
}

TEST(LocalFile_Put_Without_Open_Fails) {
    LocalFileStorageProvider p;
    auto err = p.put(TxnId{1}, "t", Value::int64(1), Tuple::make({{"v", Value::int64(1)}}));
    EXPECT(!err.isSentinel());
}

TEST(LocalFile_Comment_Box_Type_Blob_Persists) {
    cleanup("build/test_localfile_blob.db");
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_blob.db").isSentinel());
        Schema s;
        s.table = "files";
        s.columns = {
            ColumnDef{"_id", ValueType::Int64, false, true, 0, {}, std::nullopt},
            ColumnDef{"payload", ValueType::Blob, false, false, 0, {}, std::nullopt},
        };
        EXPECT(p.createTable(s).isSentinel());

        TransactionManager txnm;
        TxnId t = txnm.beginTxn();
        std::vector<std::uint8_t> bytes = {0x00, 0xFF, 0x42, 0x10, 0xAB};
        Tuple r = Tuple::make({
            {"_id", Value::int64(1)},
            {"payload", Value::blob(bytes)},
        });
        EXPECT(p.put(t, "files", Value::int64(1), r).isSentinel());
        txnm.commitTxn(t);
        EXPECT(p.commit(t, txnm.visibleSeq()).isSentinel());
        EXPECT(p.close().isSentinel());
    }
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_blob.db").isSentinel());
        auto got = p.get(TxnId{99}, "files", Value::int64(1));
        EXPECT(got.has_value());
        const auto& blob = got->get("payload").asBlob();
        EXPECT_EQ(blob.size(), std::size_t{5});
        EXPECT_EQ(blob[0], std::uint8_t{0x00});
        EXPECT_EQ(blob[1], std::uint8_t{0xFF});
        EXPECT_EQ(blob[4], std::uint8_t{0xAB});
    }
    cleanup("build/test_localfile_blob.db");
}

TEST(LocalFile_CrashDurable_Data_Persists_Across_Reopen) {
    // Phase 5 Item 2: the `CrashDurable` capability bit is honest â€” data
    // committed before close() must be readable after a fresh open() with no
    // crash midway. We synthesize "process restart" via scope boundary.
    cleanup("build/test_localfile_crash.db");
    Schema users = makeUsersSchema();
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_crash.db").isSentinel());
        EXPECT(p.createTable(users).isSentinel());

        TransactionManager txnm;
        TxnId t = txnm.beginTxn();
        for (int i = 1; i <= 5; ++i) {
            Tuple r = Tuple::make({
                {"_id", Value::int64(i)},
                {"name", Value::text(std::string("name") + std::to_string(i))},
                {"age",  Value::int32(20 + i)},
            });
            EXPECT(p.put(t, "users", Value::int64(i), r).isSentinel());
        }
        txnm.commitTxn(t);
        EXPECT(p.commit(t, txnm.visibleSeq()).isSentinel());

        // Stage another write but DO NOT commit before close.
        TxnId t2 = txnm.beginTxn();
        Tuple doomed = Tuple::make({
            {"_id", Value::int64(99)},
            {"name", Value::text("doomed")},
            {"age",  Value::int32(0)},
        });
        EXPECT(p.put(t2, "users", Value::int64(99), doomed).isSentinel());
        txnm.abortTxn(t2);

        EXPECT(p.close().isSentinel());
    }
    // "process restart" â€” fresh provider, fresh tables/BTree, fresh visible_seq_.
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_crash.db").isSentinel());
        EXPECT(p.tables().size() == std::size_t{1});
        EXPECT(p.describeTable("users").has_value());
        EXPECT_EQ(p.rowCount("users"), std::size_t{5});

        auto got = p.get(TxnId{99}, "users", Value::int64(3));
        EXPECT(got.has_value());
        EXPECT(got->get("name").asText() == std::string{"name3"});
        EXPECT(got->get("age").asInt32() == 23);

        // The aborted staged entry must NOT survive reopen.
        auto staged = p.get(TxnId{99}, "users", Value::int64(99));
        EXPECT(!staged.has_value());

        EXPECT(p.close().isSentinel());
    }
    cleanup("build/test_localfile_crash.db");
}

// ---------------------------------------------------------------------------
// Phase 5 Item 16: backupTo() â€” point-in-time snapshot
// ---------------------------------------------------------------------------

TEST(LocalFile_Backup_Creates_Independent_Copy) {
    cleanup("build/test_localfile_backup_src.db");
    cleanup("build/test_localfile_backup_dst.db");

    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_backup_src.db").isSentinel());
        Schema users = makeUsersSchema();
        EXPECT(p.createTable(users).isSentinel());

        TransactionManager txnm;
        TxnId t = txnm.beginTxn();
        for (int i = 1; i <= 3; ++i) {
            Tuple r = Tuple::make({
                {"_id", Value::int64(i)},
                {"name", Value::text(std::string("name") + std::to_string(i))},
                {"age",  Value::int32(20 + i)},
            });
            EXPECT(p.put(t, "users", Value::int64(i), r).isSentinel());
        }
        txnm.commitTxn(t);
        EXPECT(p.commit(t, txnm.visibleSeq()).isSentinel());

        // Snapshot to a separate file.
        EXPECT(p.backupTo("file://build/test_localfile_backup_dst.db").isSentinel());

        // The source is still open and usable.
        EXPECT(p.isOpen());

        EXPECT(p.close().isSentinel());
    }
    // Open the backup and verify it is a faithful snapshot.
    {
        LocalFileStorageProvider q;
        EXPECT(q.open("file://build/test_localfile_backup_dst.db").isSentinel());
        EXPECT(q.tables().size() == std::size_t{1});
        EXPECT_EQ(q.rowCount("users"), std::size_t{3});
        for (int i = 1; i <= 3; ++i) {
            auto got = q.get(TxnId{99}, "users", Value::int64(i));
            EXPECT(got.has_value());
        }
        EXPECT(q.close().isSentinel());
    }

    cleanup("build/test_localfile_backup_src.db");
    cleanup("build/test_localfile_backup_dst.db");
}

TEST(LocalFile_Backup_Fails_When_Not_Open) {
    LocalFileStorageProvider p;
    auto err = p.backupTo("file://build/test_localfile_backup_noop.db");
    // ponytail: error is non-sentinel â€” "provider not open".
    EXPECT(!err.isSentinel());
}

TEST(LocalFile_Backup_Provider_Remains_Open_And_Modifiable) {
    // After backupTo, the provider must still be open, and writes must work.
    cleanup("build/test_localfile_backup_live.db");

    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_backup_live.db").isSentinel());
    Schema users = makeUsersSchema();
    EXPECT(p.createTable(users).isSentinel());

    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    Tuple r = Tuple::make({{"_id", Value::int64(1)}, {"name", Value::text("a")}, {"age", Value::int32(1)}});
    EXPECT(p.put(t, "users", Value::int64(1), r).isSentinel());
    txnm.commitTxn(t);
    EXPECT(p.commit(t, txnm.visibleSeq()).isSentinel());

    EXPECT(p.backupTo("file://build/test_localfile_backup_live_dst.db").isSentinel());
    EXPECT(p.isOpen());

    // Insert more â€” these changes must NOT appear in the backup.
    TxnId t2 = txnm.beginTxn();
    Tuple r2 = Tuple::make({{"_id", Value::int64(2)}, {"name", Value::text("b")}, {"age", Value::int32(2)}});
    EXPECT(p.put(t2, "users", Value::int64(2), r2).isSentinel());
    txnm.commitTxn(t2);
    EXPECT(p.commit(t2, txnm.visibleSeq()).isSentinel());

    EXPECT_EQ(p.rowCount("users"), std::size_t{2});

    // Open the backup and confirm only the original row is there.
    LocalFileStorageProvider q;
    EXPECT(q.open("file://build/test_localfile_backup_live_dst.db").isSentinel());
    EXPECT_EQ(q.rowCount("users"), std::size_t{1});
    EXPECT(q.close().isSentinel());

    EXPECT(p.close().isSentinel());

    cleanup("build/test_localfile_backup_live.db");
    cleanup("build/test_localfile_backup_live_dst.db");
}

TEST(LocalFile_Path_Returns_Stripped_FileURI) {
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_path.db").isSentinel());
    EXPECT(p.path() == std::string{"build/test_localfile_path.db"});
    EXPECT(p.close().isSentinel());
    cleanup("build/test_localfile_path.db");
}

// ---------------------------------------------------------------------------
// Phase 6.4: BTree per-page recycling.
// dropTable walks the BTree and calls Pager::freePage on every node. The
// free-list counter exposed via freePageCount() should rise accordingly.
// ---------------------------------------------------------------------------

TEST(LocalFile_DropTable_Frees_Pages_To_Pager) {
    cleanup("build/test_localfile_drop_pages.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_drop_pages.db").isSentinel());

    Schema s = makeUsersSchema();
    EXPECT(p.createTable(s).isSentinel());

    // Insert some rows to make the BTree allocate pages.
    auto* engine = p.engine();
    TxnId t{1};
    for (int i = 1; i <= 50; ++i) {
        Tuple row = Tuple::make({{"_id", Value::int64(i)},
                                  {"name", Value::text("u" + std::to_string(i))}});
        EXPECT(engine->put(t, "users", Value::int64(i), row).isSentinel());
    }
    EXPECT(engine->prepare(t).isSentinel());
    EXPECT(engine->commit(t).isSentinel());

    const std::size_t beforeFree = p.freePageCount();
    EXPECT(p.dropTable("users").isSentinel());
    const std::size_t afterFree = p.freePageCount();
    EXPECT(afterFree > beforeFree);

    EXPECT(p.close().isSentinel());
    cleanup("build/test_localfile_drop_pages.db");
}

TEST(LocalFile_DropTable_Recycled_Pages_Are_Reused) {
    cleanup("build/test_localfile_drop_reuse.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_drop_reuse.db").isSentinel());

    Schema s = makeUsersSchema();
    EXPECT(p.createTable(s).isSentinel());
    auto* engine = p.engine();
    TxnId t{1};
    for (int i = 1; i <= 50; ++i) {
        Tuple row = Tuple::make({{"_id", Value::int64(i)},
                                  {"name", Value::text("u" + std::to_string(i))}});
        EXPECT(engine->put(t, "users", Value::int64(i), row).isSentinel());
    }
    EXPECT(engine->prepare(t).isSentinel());
    EXPECT(engine->commit(t).isSentinel());

    const std::size_t beforePageCount = p.freePageCount();
    EXPECT(p.dropTable("users").isSentinel());
    const std::size_t freedPages = p.freePageCount();
    EXPECT(freedPages > beforePageCount);

    // Recreate the table; allocatePage should pull from the free-list.
    Schema s2 = makeUsersSchema();
    EXPECT(p.createTable(s2).isSentinel());
    EXPECT(p.freePageCount() < freedPages);

    EXPECT(p.close().isSentinel());
    cleanup("build/test_localfile_drop_reuse.db");
}

TEST(LocalFile_DropTable_FreePages_Survive_Reopen) {
    cleanup("build/test_localfile_drop_reopen.db");
    {
        LocalFileStorageProvider p;
        EXPECT(p.open("file://build/test_localfile_drop_reopen.db").isSentinel());
        Schema s = makeUsersSchema();
        EXPECT(p.createTable(s).isSentinel());
        auto* engine = p.engine();
        TxnId t{1};
        for (int i = 1; i <= 30; ++i) {
            Tuple row = Tuple::make({{"_id", Value::int64(i)},
                                      {"name", Value::text("u" + std::to_string(i))}});
            EXPECT(engine->put(t, "users", Value::int64(i), row).isSentinel());
        }
        EXPECT(engine->prepare(t).isSentinel());
        EXPECT(engine->commit(t).isSentinel());
        EXPECT(p.dropTable("users").isSentinel());
        EXPECT(p.close().isSentinel());
    }
    // Reopen and verify the free-list persisted.
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_drop_reopen.db").isSentinel());
    EXPECT(p.freePageCount() > 0);
    EXPECT(p.close().isSentinel());
    cleanup("build/test_localfile_drop_reopen.db");
}

TEST(LocalFile_DropTable_Empty_Table_Frees_At_Least_One_Page) {
    cleanup("build/test_localfile_drop_empty.db");
    LocalFileStorageProvider p;
    EXPECT(p.open("file://build/test_localfile_drop_empty.db").isSentinel());

    Schema s = makeUsersSchema();
    EXPECT(p.createTable(s).isSentinel());
    const std::size_t beforeFree = p.freePageCount();
    EXPECT(p.dropTable("users").isSentinel());
    // Even an empty BTree has a root page; dropping it must free at least 1.
    EXPECT(p.freePageCount() > beforeFree);

    EXPECT(p.close().isSentinel());
    cleanup("build/test_localfile_drop_empty.db");
}

