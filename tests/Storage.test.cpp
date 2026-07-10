#include "test_framework.hpp"

#include <vector>

#include "atomdb/contracts/IStorageEngine.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/storage/InMemoryStorageEngine.hpp"

using namespace atomdb;

TEST(Storage_Put_And_Get_Round_Trip) {
    InMemoryStorageEngine s;
    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    Tuple row = Tuple::make({{"name", Value::text("a")}, {"_id", Value::integer(1)}});
    DbError err = s.put(t, "users", Value::integer(1), row);
    EXPECT(err.isSentinel());
    auto got = s.get(t, "users", Value::integer(1));
    EXPECT(got.has_value());
    EXPECT(got->get("name").asText() == "a");

    txnm.commitTxn(t);
    s.commit(t, txnm.visibleSeq());

    // Subsequent reads (a new txn id) should still see the committed record.
    EXPECT(s.get(txnm.beginTxn(), "users", Value::integer(1)).has_value());
}

TEST(Storage_Auto_Key_Assigns_Int64) {
    InMemoryStorageEngine s;
    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    Tuple row = Tuple::make({{"name", Value::text("a")}});
    DbError err = s.put(t, "users", Value::null(), row);
    EXPECT(err.isSentinel());
    auto got = s.get(t, "users", Value::integer(1));
    EXPECT(got.has_value());
    EXPECT(got->has("_id"));
    EXPECT_EQ(got->get("_id").asInt64(), 1);
}

TEST(Storage_Overwrite_Appends_Version) {
    InMemoryStorageEngine s;
    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    Tuple row1 = Tuple::make({{"name", Value::text("a")}, {"_id", Value::integer(1)}});
    s.put(t, "users", Value::integer(1), row1);
    txnm.commitTxn(t);
    s.commit(t, txnm.visibleSeq());

    // Uncommitted overwrite.
    TxnId t2 = txnm.beginTxn();
    Tuple row2 = Tuple::make({{"name", Value::text("b")}, {"_id", Value::integer(1)}});
    s.put(t2, "users", Value::integer(1), row2);
    // A reader outside this txn should still see row1 (committed).
    auto got_other = s.get(txnm.beginTxn(), "users", Value::integer(1));
    EXPECT(got_other->get("name").asText() == "a");
    // Inside the active uncommitted txn, row2 must be visible (RMW).
    auto got_self = s.get(t2, "users", Value::integer(1));
    EXPECT(got_self->get("name").asText() == "b");
}

TEST(Storage_Remove_Creates_Tombstone) {
    InMemoryStorageEngine s;
    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    Tuple row = Tuple::make({{"name", Value::text("a")}, {"_id", Value::integer(1)}});
    s.put(t, "users", Value::integer(1), row);
    txnm.commitTxn(t);
    s.commit(t, txnm.visibleSeq());

    // Begin a new txn, remove, check inserts sees the tombstone.
    TxnId t2 = txnm.beginTxn();
    DbError err = s.remove(t2, "users", Value::integer(1));
    EXPECT(err.isSentinel());

    // Outside the txn, still visible (not committed yet).
    auto got_other = s.get(txnm.beginTxn(), "users", Value::integer(1));
    EXPECT(got_other.has_value());

    // Inside the deleting txn, the key is "deleted" for reads.
    auto got_self = s.get(t2, "users", Value::integer(1));
    EXPECT(!got_self.has_value());
}

TEST(Storage_Scan_Iterates_All_Rows) {
    InMemoryStorageEngine s;
    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    for (std::int64_t i = 1; i <= 5; ++i) {
        Tuple row = Tuple::make({{"k", Value::integer(i)}});
        s.put(t, "tbl", Value::integer(i), row);
    }
    txnm.commitTxn(t);
    s.commit(t, txnm.visibleSeq());

    std::vector<std::int64_t> seen;
    s.scan(txnm.beginTxn(), "tbl", [&](const Tuple& r) {
        seen.push_back(r.get("k").asInt64());
    });
    EXPECT_EQ(seen.size(), std::size_t{5});
    {
        const std::vector<std::int64_t> want{1, 2, 3, 4, 5};
        EXPECT(seen == want);
    }
}

TEST(Storage_Abort_Removes_Uncommitted_Staged) {
    InMemoryStorageEngine s;
    TransactionManager txnm;
    TxnId t = txnm.beginTxn();
    Tuple row = Tuple::make({{"k", Value::integer(7)}, {"_id", Value::integer(7)}});
    s.put(t, "tbl", Value::integer(7), row);
    // abort before commit — record should disappear.
    txnm.abortTxn(t);
    DbError err = s.abort(t);
    EXPECT(err.isSentinel());
    EXPECT(!s.get(txnm.beginTxn(), "tbl", Value::integer(7)).has_value());
    EXPECT_EQ(s.rowCount("tbl"), std::size_t{0});
}
