// btree_probe.cpp — reproduction probe for AUDIT.md item B13
// (origin: 2nd-pass audit P1-1; escalated by the independent reviewer).
//
// Defect: BTree::splitLeaf (src/BTree.cpp) writes the new right page with
// right_sibling = 0 instead of the split leaf's old right_sibling. Any split
// of a non-rightmost leaf orphans every leaf page to its right. scan() and
// scanAll() walk the right_sibling chain, so they silently lose the orphaned
// pages — while point get() still finds every key, because findLeaf() routes
// through the internal nodes.
//
// Why the unit suite stays green: tests/BTree.test.cpp only ever inserts
// ascending keys (1..200 in BTree_Put_Many_Rows_Triggers_Splits), and an
// ascending insert always splits the current rightmost leaf, whose
// right_sibling is already 0 — the exact case where the bug is invisible.
//
// This probe inserts the same 400 keys in three orders (ascending,
// descending, random with a fixed seed) and reports, per order:
//   scan=<rows visible to scan()>/<inserted>   (point-gets find <n>/<inserted>)
//
// Expected output AFTER the splitLeaf fix: all three orders scan=400/400.
// Broken output at HEAD 69cb019: ascending scans full; descending and random
// scan only a small fraction (the first split's survivors), point-gets 400/400.
//
// Build (from repo root):
//   g++ -std=c++2b -Iincludes tools/probes/btree_probe.cpp src/BTree.cpp \
//       src/Pager.cpp src/BuddyPageAllocator.cpp -o build/btree_probe
// Run:
//   ./build/btree_probe        (writes/deletes probe_btree.dat in the cwd)
// Exit code: 0 = all orders fully scannable (fixed), 1 = scan lost rows (bug
// present), 2 = unexpected put() failure.

#include "opendb/storage/BTree.hpp"
#include "opendb/storage/Pager.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

opendb::Tuple rowFor(int k) {
    opendb::Tuple t;
    t.set("id", opendb::Value::int64(k));
    t.set("name", opendb::Value::text("row-" + std::to_string(k)));
    t.set("age", opendb::Value::int64(30 + (k % 10)));
    return t;
}

int runOrder(const std::string& label, const std::vector<int>& keys) {
    std::remove("probe_btree.dat");
    opendb::Pager pager("probe_btree.dat");
    opendb::BTree bt(pager);
    bt.createNew();

    const opendb::TxnId txn{999};
    const std::uint64_t commitSeq = 1;  // committed => visible to every reader
    const std::uint64_t visibleSeq = 100;

    for (int k : keys) {
        if (!bt.put(opendb::Value::int64(k), commitSeq, txn.value(), false, rowFor(k))) {
            std::printf("%-10s put(%d) unexpectedly returned false\n", label.c_str(), k);
            return 2;
        }
    }

    // What a table scan (SELECT) sees: walks the right_sibling chain.
    int scanned = 0;
    bt.scan(txn, visibleSeq, [&](const opendb::Value&, const opendb::BTree::LookupResult& r) {
        if (!r.tombstone) ++scanned;
        return true;
    });

    // What a point lookup (_id = k) sees: routes via the internal nodes.
    int got = 0;
    for (int k : keys) {
        auto r = bt.get(opendb::Value::int64(k), txn, visibleSeq);
        if (r.found && !r.tombstone) ++got;
    }

    std::printf("%-10s scan=%d/%d%s   (point-gets find %d/%d)\n",
                label.c_str(), scanned, static_cast<int>(keys.size()),
                scanned == static_cast<int>(keys.size()) ? " OK" : "",
                got, static_cast<int>(keys.size()));
    return scanned == static_cast<int>(keys.size()) ? 0 : 1;
}

} // namespace

int main() {
    constexpr int N = 400;
    std::vector<int> asc;
    for (int i = 1; i <= N; ++i) asc.push_back(i);
    std::vector<int> desc(asc.rbegin(), asc.rend());
    std::vector<int> rnd(asc);
    std::shuffle(rnd.begin(), rnd.end(), std::mt19937_64(12345));

    int rc = 0;
    rc |= runOrder("ascending", asc);
    rc |= runOrder("descending", desc);
    rc |= runOrder("random", rnd);
    std::printf("%s\n",
                rc == 0 ? "PASS: all three orders fully scannable"
                        : "FAIL: scan lost rows on at least one order (AUDIT B13)");
    return rc;
}
