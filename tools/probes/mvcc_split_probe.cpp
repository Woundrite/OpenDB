// mvcc_split_probe.cpp — reproduction probe for AUDIT.md item B15
// (origin: 2nd-pass P1-2; related: B13, fixed separately).
//
// Question: can one key's version group (key ASC, commitSeq DESC) straddle a
// page split so that get() — which scans only the leaf findLeaf() routes to —
// misses an older *visible* version stranded on the other page?
//
// Construction (deterministic; tiny payloads keep the byte cap dormant so the
// count cap fires the split at exactly 65 entries, mid = 32):
//   - 31 left keys 1..31 (commitSeq=1)
//   - key 500: (seq=5, committed) at index 31 and (seq=0, staged by ANOTHER
//     txn) at index 32 — the group straddles the split point
//   - right keys 501.. inserted one at a time; the 32nd right key makes the
//     65th entry and triggers the split
//   Split outcome: left page = [1..31, (500,5)]; right page = [(500,0),
//   501..532]; promoted separator = 500.
//
// A reader with visible_seq=10 should see the (500,5) version. findLeaf(500)
// routes it to the RIGHT page (500 >= separator 500), which holds only the
// invisible staged (500,0) — so get() reports NOT FOUND while a visible
// version sits on the left page. scan() DOES see key 500 (it walks both
// pages): point-get and scan disagree, and the row is missing for lookups.
//
// Build (from repo root):
//   g++ -std=c++2b -Iincludes tools/probes/mvcc_split_probe.cpp src/BTree.cpp \
//       src/Pager.cpp src/BuddyPageAllocator.cpp -o build/mvcc_split_probe
// Run:
//   ./build/mvcc_split_probe   (writes/deletes probe_mvcc.dat in the cwd)
// Exit code: 1 = B15 reproduced (bug present); 0 = not reproduced; 2 = inconclusive.

#include "opendb/storage/BTree.hpp"
#include "opendb/storage/Pager.hpp"

#include <cstdio>
#include <string>

namespace {

opendb::Tuple tiny(int v) {
    opendb::Tuple t;
    t.set("v", opendb::Value::int64(v));
    return t;
}

} // namespace

int main() {
    std::remove("probe_mvcc.dat");
    opendb::Pager pager("probe_mvcc.dat");
    opendb::BTree bt(pager);
    bt.createNew();

    const opendb::TxnId committer{1};
    const opendb::TxnId other{2};    // stages the invisible version
    const opendb::TxnId reader{3};
    constexpr int kM = 500;

    // 31 left keys (indices 0..30).
    for (int k = 1; k <= 31; ++k) {
        bt.put(opendb::Value::int64(k), 1, committer.value(), false, tiny(k));
    }
    // Key 500: one committed version (visible at visible_seq >= 5) at index
    // 31, and one version staged by ANOTHER txn (commitSeq == 0 — sorts LAST
    // in the group) at index 32.
    bt.put(opendb::Value::int64(kM), 5, committer.value(), false, tiny(5));
    bt.put(opendb::Value::int64(kM), 0, other.value(), false, tiny(0));

    // Right keys until the count cap fires the split at 65 entries (the
    // split allocates a new leaf AND a new internal root: +2 pages).
    const std::uint32_t pages_before = pager.pageCount();
    for (int k = 501; k <= 560; ++k) {
        bt.put(opendb::Value::int64(k), 1, committer.value(), false, tiny(k));
        if (pager.pageCount() >= pages_before + 2) break;
    }
    const bool split_occurred = pager.pageCount() >= pages_before + 2;

    // Reader with visible_seq = 10: (500,5) is visible; (500,0) is not.
    auto rg = bt.get(opendb::Value::int64(kM), reader, 10);
    // The staging txn sees its own write (read-your-own-writes).
    auto og = bt.get(opendb::Value::int64(kM), other, 0);
    // scan() sees key 500 iff a visible version is reachable via the chain.
    bool scan_sees = false;
    bt.scan(reader, 10, [&](const opendb::Value& key, const opendb::BTree::LookupResult& r) {
        if (!r.tombstone && key.asInt64() == kM) scan_sees = true;
        return true;
    });

    std::printf("split occurred: %s (pages %u -> %u)\n",
                split_occurred ? "yes" : "NO",
                static_cast<unsigned>(pages_before),
                static_cast<unsigned>(pager.pageCount()));
    if (rg.found) {
        std::printf("reader get(500): found seq=%llu\n",
                    static_cast<unsigned long long>(rg.commitSeq));
    } else {
        std::printf("reader get(500): NOT FOUND\n");
    }
    std::printf("scan sees 500   : %s\n", scan_sees ? "yes" : "no");
    if (og.found) {
        std::printf("owner  get(500): found seq=%llu\n",
                    static_cast<unsigned long long>(og.commitSeq));
    } else {
        std::printf("owner  get(500): NOT FOUND\n");
    }

    if (split_occurred && !rg.found && scan_sees) {
        std::printf("VERDICT: B15 REPRODUCED — get() misses the visible (500,5) "
                    "version stranded on the left page while scan() finds it\n");
        return 1;
    }
    if (rg.found && rg.commitSeq == 5) {
        std::printf("VERDICT: not reproduced — reader correctly sees (500,5)\n");
        return 0;
    }
    std::printf("VERDICT: inconclusive — check diagnostics above\n");
    return 2;
}
