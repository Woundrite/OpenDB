#include "test_framework.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "opendb/storage/BTree.hpp"
#include "opendb/storage/Pager.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/TxnId.hpp"
#include "opendb/types/Value.hpp"

using namespace opendb;

namespace {
constexpr const char* kTmpDir = "build";

std::string tmpFile(const std::string& tag) {
    return std::string(kTmpDir) + "/test_btree_" + tag + ".db";
}
void cleanup(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
} // namespace

// ---- basic put + get -------------------------------------------------------

TEST(BTree_CreateNew_Allocates_Root) {
    cleanup(tmpFile("create"));
    Pager p(tmpFile("create"));
    BTree bt(p);
    Pager::PageId root = bt.createNew();
    EXPECT(root != 0);
    EXPECT_EQ(bt.rootPageId(), root);
}

TEST(BTree_Put_And_Get_Single_Row) {
    cleanup(tmpFile("single"));
    Pager p(tmpFile("single"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    Tuple row = Tuple::make({{"name", Value::text("nikhil")}, {"age", Value::int32(30)}});
    EXPECT(bt.put(Value::int64(1), 0, t.value(), false, row));

    auto r = bt.get(Value::int64(1), t, 0);
    EXPECT(r.found);
    EXPECT_EQ(r.commitSeq, std::uint64_t{0});
    EXPECT(r.payload.get("name").asText() == "nikhil");
    EXPECT_EQ(r.payload.get("age").asInt32(), 30);
}

TEST(BTree_Get_Missing_Key_Returns_Not_Found) {
    cleanup(tmpFile("missing"));
    Pager p(tmpFile("missing"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    auto r = bt.get(Value::int64(42), t, 0);
    EXPECT(!r.found);
}

TEST(BTree_Staged_Visible_Only_To_Owning_Txn) {
    cleanup(tmpFile("stagedvisible"));
    Pager p(tmpFile("stagedvisible"));
    BTree bt(p);
    bt.createNew();

    TxnId a{1}, b{2};
    Tuple row = Tuple::make({{"v", Value::int64(100)}});
    bt.put(Value::int64(7), 0, a.value(), false, row);

    // Owner sees it.
    auto ra = bt.get(Value::int64(7), a, 999999);
    EXPECT(ra.found);

    // Other txn does NOT see it (commitSeq=0, not owning).
    auto rb = bt.get(Value::int64(7), b, 999999);
    EXPECT(!rb.found);
}

TEST(BTree_Committed_Visible_To_All_After_Seq) {
    cleanup(tmpFile("committed"));
    Pager p(tmpFile("committed"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    Tuple row = Tuple::make({{"v", Value::int64(100)}});
    bt.put(Value::int64(7), 42, t.value(), false, row); // commitSeq=42

    // Reader with low visible_seq cannot see it.
    auto r1 = bt.get(Value::int64(7), t, 10);
    EXPECT(!r1.found);

    // Reader with high visible_seq sees it.
    auto r2 = bt.get(Value::int64(7), t, 100);
    EXPECT(r2.found);
    EXPECT_EQ(r2.commitSeq, std::uint64_t{42});
}

TEST(BTree_Tombstone_Returns_Found_With_Tombstone_True) {
    cleanup(tmpFile("tombstone"));
    Pager p(tmpFile("tombstone"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    Tuple empty;
    EXPECT(bt.put(Value::int64(7), 0, t.value(), true, empty));

    auto r = bt.get(Value::int64(7), t, 0);
    EXPECT(r.found);
    EXPECT(r.tombstone);
}

TEST(BTree_Multiple_Versions_Ordered_CommitSeq_DESC) {
    cleanup(tmpFile("versions"));
    Pager p(tmpFile("versions"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    Tuple r1 = Tuple::make({{"v", Value::int64(1)}});
    Tuple r2 = Tuple::make({{"v", Value::int64(2)}});
    Tuple r3 = Tuple::make({{"v", Value::int64(3)}});
    bt.put(Value::int64(7), 10, t.value(), false, r1);
    bt.put(Value::int64(7), 20, t.value(), false, r2);
    bt.put(Value::int64(7), 30, t.value(), false, r3);

    // Highest commitSeq <= visible_seq wins.
    auto a = bt.get(Value::int64(7), t, 25);
    EXPECT(a.found);
    EXPECT_EQ(a.commitSeq, std::uint64_t{20});

    auto b = bt.get(Value::int64(7), t, 100);
    EXPECT(b.found);
    EXPECT_EQ(b.commitSeq, std::uint64_t{30});
}

TEST(BTree_Scan_Visits_All_Keys_In_Order) {
    cleanup(tmpFile("scan"));
    Pager p(tmpFile("scan"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    // Insert keys 10, 3, 7, 1, 5, 9 in random order.
    for (int k : {10, 3, 7, 1, 5, 9}) {
        Tuple row = Tuple::make({{"k", Value::int64(k)}});
        EXPECT(bt.put(Value::int64(k), 1, t.value(), false, row));
    }

    std::vector<std::int64_t> keys;
    bt.scan(t, 100, [&](const Value& key, const BTree::LookupResult&) {
        keys.push_back(key.asInt64());
        return true;
    });
    const std::vector<std::int64_t> want{1, 3, 5, 7, 9, 10};
    EXPECT(keys == want);
}

TEST(BTree_Scan_Empty_Tree_Calls_Callback_Nothing) {
    cleanup(tmpFile("scansEmpty"));
    Pager p(tmpFile("scansEmpty"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    int count = 0;
    bt.scan(t, 0, [&](const Value&, const BTree::LookupResult&) {
        ++count;
        return true;
    });
    EXPECT_EQ(count, 0);
}

TEST(BTree_Put_Many_Rows_Triggers_Splits) {
    cleanup(tmpFile("split"));
    Pager p(tmpFile("split"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    // Insert 200 rows to guarantee at least one split.
    for (int i = 1; i <= 200; ++i) {
        Tuple row = Tuple::make({{"k", Value::int64(i)}});
        EXPECT(bt.put(Value::int64(i), 1, t.value(), false, row));
    }

    // Verify all keys are reachable via scan.
    std::vector<std::int64_t> keys;
    bt.scan(t, 100, [&](const Value& key, const BTree::LookupResult&) {
        keys.push_back(key.asInt64());
        return true;
    });
    EXPECT_EQ(keys.size(), std::size_t{200});

    // Verify random lookup by key.
    for (int i : {1, 50, 100, 150, 200}) {
        auto r = bt.get(Value::int64(i), t, 100);
        EXPECT(r.found);
        EXPECT_EQ(r.payload.get("k").asInt64(), std::int64_t(i));
    }
}

TEST(BTree_Duplicate_Put_Same_Key_CommitSeq_TxnId_Returns_False) {
    cleanup(tmpFile("dup"));
    Pager p(tmpFile("dup"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    Tuple row = Tuple::make({{"v", Value::int64(1)}});
    EXPECT(bt.put(Value::int64(7), 0, t.value(), false, row));
    EXPECT(!bt.put(Value::int64(7), 0, t.value(), false, row)); // duplicate -> false
}

TEST(BTree_Persistence_Across_Pager_Instances) {
    cleanup(tmpFile("persist"));
    {
        Pager p(tmpFile("persist"));
        BTree bt(p);
        bt.createNew();
        TxnId t{1};
        for (int i = 1; i <= 10; ++i) {
            Tuple row = Tuple::make({{"k", Value::int64(i)}});
            EXPECT(bt.put(Value::int64(i), 1, t.value(), false, row));
        }
        bt.flush();
    }
    // Re-open the file with a fresh Pager, rebuild BTree using the saved root.
    {
        Pager p(tmpFile("persist"));
        // Read root page ID from page 1 (the saved root we wrote via createNew returns page 1).
        // The BTree header doesn't persist root, so for v1 we assume page 1.
        // Reconstruct the BTree — but we don't have the root_page_id across instances
        // unless we serialize it. Verify the data is at least on disk by checking pageCount.
        EXPECT(p.pageCount() > 1);
        // Note: full reload requires metadata layer (handled by LocalFileStorageProvider).
    }
}

TEST(BTree_Text_Keys_Sort_Lexicographically) {
    cleanup(tmpFile("textkeys"));
    Pager p(tmpFile("textkeys"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    for (const char* name : {"charlie", "alice", "bob", "dave"}) {
        std::string sname = name;
        Tuple row = Tuple::make({{"name", Value::text(sname)}});
        EXPECT(bt.put(Value::text(sname), 1, t.value(), false, row));
    }

    std::vector<std::string> keys;
    bt.scan(t, 100, [&](const Value& key, const BTree::LookupResult&) {
        keys.push_back(key.asText());
        return true;
    });
    EXPECT(keys == (std::vector<std::string>{"alice", "bob", "charlie", "dave"}));
}

TEST(BTree_Scan_Stops_When_Callback_Returns_False) {
    cleanup(tmpFile("scanstop"));
    Pager p(tmpFile("scanstop"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    for (int i = 1; i <= 20; ++i) {
        Tuple row = Tuple::make({{"k", Value::int64(i)}});
        EXPECT(bt.put(Value::int64(i), 1, t.value(), false, row));
    }

    int count = 0;
    bt.scan(t, 100, [&](const Value&, const BTree::LookupResult&) {
        ++count;
        return count < 5; // stop after 5 entries
    });
    EXPECT_EQ(count, 5);
}

// ---- chunk 3 regression: sibling-chain preservation across splits (B13/B14) ----
// The original suite only inserted ascending keys, so every split happened at
// the rightmost leaf, where right_sibling was already 0 — hiding the severed
// chain. These tests force splits at non-rightmost positions; each one fails
// on the unfixed code and passes after the splitLeaf fix.

namespace {

std::vector<int> shuffledKeys(int n, std::uint64_t seed) {
    std::vector<int> keys;
    for (int i = 1; i <= n; ++i) keys.push_back(i);
    std::shuffle(keys.begin(), keys.end(), std::mt19937_64(seed));
    return keys;
}

Tuple tinyRow(int v) {
    return Tuple::make({{"v", Value::int64(v)}});
}

// Scan must report every key exactly once, in strictly ascending order.
bool scanIsCompleteAndSorted(BTree& bt, const TxnId& t, std::size_t expected) {
    std::size_t count = 0;
    std::int64_t last = 0; // all keys are >= 1
    bool sorted = true;
    bt.scan(t, 100, [&](const Value& key, const BTree::LookupResult& r) {
        if (r.tombstone) return true;
        ++count;
        std::int64_t k = key.asInt64();
        if (k <= last) sorted = false;
        last = k;
        return true;
    });
    return count == expected && sorted;
}

} // namespace

TEST(BTree_Descending_Inserts_Scan_Count_Equals_Inserted) {
    cleanup(tmpFile("desc_scan"));
    Pager p(tmpFile("desc_scan"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    for (int k = 400; k >= 1; --k) {
        EXPECT(bt.put(Value::int64(k), 1, t.value(), false, tinyRow(k)));
    }

    std::size_t scanned = 0;
    bt.scan(t, 100, [&](const Value&, const BTree::LookupResult& r) {
        if (!r.tombstone) ++scanned;
        return true;
    });
    EXPECT_EQ(scanned, std::size_t{400});
}

TEST(BTree_Shuffled_Inserts_Scan_Count_Equals_Inserted) {
    cleanup(tmpFile("shuf_scan"));
    Pager p(tmpFile("shuf_scan"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    for (int k : shuffledKeys(400, 12345)) {
        EXPECT(bt.put(Value::int64(k), 1, t.value(), false, tinyRow(k)));
    }

    std::size_t scanned = 0;
    bt.scan(t, 100, [&](const Value&, const BTree::LookupResult& r) {
        if (!r.tombstone) ++scanned;
        return true;
    });
    EXPECT_EQ(scanned, std::size_t{400});
}

TEST(BTree_Scan_Returns_Sorted_Keys_For_Every_Insert_Order) {
    TxnId t{1};

    // Ascending.
    {
        cleanup(tmpFile("scanorder"));
        Pager p(tmpFile("scanorder"));
        BTree bt(p);
        bt.createNew();
        for (int k = 1; k <= 400; ++k)
            bt.put(Value::int64(k), 1, t.value(), false, tinyRow(k));
        EXPECT(scanIsCompleteAndSorted(bt, t, 400));
    }
    // Descending.
    {
        cleanup(tmpFile("scanorder"));
        Pager p(tmpFile("scanorder"));
        BTree bt(p);
        bt.createNew();
        for (int k = 400; k >= 1; --k)
            bt.put(Value::int64(k), 1, t.value(), false, tinyRow(k));
        EXPECT(scanIsCompleteAndSorted(bt, t, 400));
    }
    // Shuffled (fixed seed).
    {
        cleanup(tmpFile("scanorder"));
        Pager p(tmpFile("scanorder"));
        BTree bt(p);
        bt.createNew();
        for (int k : shuffledKeys(400, 12345))
            bt.put(Value::int64(k), 1, t.value(), false, tinyRow(k));
        EXPECT(scanIsCompleteAndSorted(bt, t, 400));
    }
}

TEST(BTree_Shuffled_Staged_Writes_All_Reachable_By_scanAll) {
    cleanup(tmpFile("staged_reach"));
    Pager p(tmpFile("staged_reach"));
    BTree bt(p);
    bt.createNew();

    TxnId writer{77};
    for (int k : shuffledKeys(400, 777)) {
        EXPECT(bt.put(Value::int64(k), 0 /*staged*/, writer.value(), false, tinyRow(k)));
    }

    // What LocalFileStorageProvider::commit() does: scanAll() must reach
    // every staged write of the transaction.
    std::size_t reachable = 0;
    bt.scanAll([&](const BTree::RawEntry& e) {
        if (e.commitSeq == 0 && e.txnId == writer.value()) ++reachable;
        return true;
    });
    EXPECT_EQ(reachable, std::size_t{400});
}

TEST(BTree_Interleaved_Inserts_Splits_At_Mixed_Positions) {
    cleanup(tmpFile("interleaved"));
    Pager p(tmpFile("interleaved"));
    BTree bt(p);
    bt.createNew();

    TxnId t{1};
    // Phase 1: ascending batch (splits at the right edge).
    for (int k = 1; k <= 150; ++k)
        bt.put(Value::int64(k), 1, t.value(), false, tinyRow(k));
    // Phase 2: descending batch above the current range (fills the rightmost
    // leaf, then splits at interior positions as the batch grows downward).
    for (int k = 600; k >= 451; --k)
        bt.put(Value::int64(k), 1, t.value(), false, tinyRow(k));
    // Phase 3: interleaved low/high pairs filling the middle from both ends
    // (splits at mixed positions).
    for (int k = 0; k < 150; ++k) {
        bt.put(Value::int64(151 + k), 1, t.value(), false, tinyRow(151 + k));
        bt.put(Value::int64(450 - k), 1, t.value(), false, tinyRow(450 - k));
    }

    EXPECT(scanIsCompleteAndSorted(bt, t, 600));

    // Point-gets still find every key (they never depended on the chain).
    int got = 0;
    for (int k = 1; k <= 600; ++k) {
        auto r = bt.get(Value::int64(k), t, 100);
        if (r.found && !r.tombstone) ++got;
    }
    EXPECT_EQ(got, 600);
}
