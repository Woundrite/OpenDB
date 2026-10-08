// commit_probe.cpp — reproduction probe for AUDIT.md item B14
// (origin: independent reviewer escalation of 2nd-pass P1-1; see also B13).
//
// Defect: LocalFileStorageProvider::commit() (includes/opendb/storage/
// LocalFileStorageProvider.hpp, "commit(TxnId, visibleSeq)") finds a
// transaction's staged writes with BTree::scanAll(), and scanAll() walks the
// same right_sibling chain that splitLeaf severs (B13). Staged writes on
// orphaned pages are therefore never collected, never re-put with the commit
// sequence, and stay at commitSeq==0 — visible only to a transaction that has
// already finished. They are permanently lost: this is committed-data loss,
// not just rows missing from SELECT.
//
// This probe mirrors the provider's commit path against a bare BTree:
//   1. stage 400 random-order writes (commitSeq=0, txnId=writer) — what
//      put() does inside a transaction;
//   2. run exactly what commit() does: scanAll() to collect the txn's staged
//      entries, then re-put each collected entry with commitSeq=1;
//   3. report how many staged writes the commit scan reached (== how many
//      were actually committed) and how many rows a subsequent reader sees.
//
// Expected output AFTER the splitLeaf fix:
//   staged=400  commit-reachable=400  owner point-gets=400/400  reader sees 400/400
// Broken output at HEAD 69cb019: commit-reachable is a small fraction of 400
// (independent reviewer's first reproduction: 78/400 random-order staged
// writes reachable, 322 never committed), so the reader permanently misses
// the unreachable rows.
//
// Build (from repo root):
//   g++ -std=c++2b -Iincludes tools/probes/commit_probe.cpp src/BTree.cpp \
//       src/Pager.cpp src/BuddyPageAllocator.cpp -o build/commit_probe
// Run:
//   ./build/commit_probe       (writes/deletes probe_commit.dat in the cwd)
// Exit code: 0 = commit scan reaches every staged write (fixed), 1 = some
// staged writes are unreachable by commit() and are lost (bug present).

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

} // namespace

int main() {
    constexpr int N = 400;
    std::vector<int> keys;
    for (int i = 1; i <= N; ++i) keys.push_back(i);
    std::shuffle(keys.begin(), keys.end(), std::mt19937_64(12345));

    std::remove("probe_commit.dat");
    opendb::Pager pager("probe_commit.dat");
    opendb::BTree bt(pager);
    bt.createNew();

    const opendb::TxnId writer{777};
    const opendb::TxnId reader{778};

    // 1. Stage all N writes, exactly as LocalFileStorageProvider::put() does
    //    inside a transaction (commitSeq == 0 marks "staged").
    for (int k : keys) {
        bt.put(opendb::Value::int64(k), 0 /*staged*/, writer.value(), false, rowFor(k));
    }

    // Every staged key is point-gettable by the owning txn (findLeaf uses the
    // internal nodes, not the sibling chain) — the writer sees all 400.
    int owner_gets = 0;
    for (int k : keys) {
        auto r = bt.get(opendb::Value::int64(k), writer, 0 /*visibleSeq*/);
        if (r.found && !r.tombstone) ++owner_gets;
    }

    // 2. Mirror LocalFileStorageProvider::commit(): collect the txn's staged
    //    entries via scanAll() (the severed chain), then re-put each with
    //    commitSeq = 1.
    std::vector<opendb::BTree::RawEntry> staged;
    bt.scanAll([&](const opendb::BTree::RawEntry& e) {
        if (e.commitSeq == 0 && e.txnId == writer.value()) {
            staged.push_back(e);
        }
        return true;
    });
    int promoted = 0;
    for (const auto& e : staged) {
        if (bt.put(e.key, 1 /*commitSeq*/, writer.value(), e.tombstone, e.payload)) {
            ++promoted;
        }
    }

    // 3. What a later reader (different txn, visibleSeq >= 1) sees.
    int reader_sees = 0;
    for (int k : keys) {
        auto r = bt.get(opendb::Value::int64(k), reader, 1 /*visibleSeq*/);
        if (r.found && !r.tombstone) ++reader_sees;
    }

    std::printf(
        "staged=%d  commit-reachable=%d  owner point-gets=%d/%d  reader sees %d/%d\n",
        N, promoted, owner_gets, N, reader_sees, N);

    if (promoted == N) {
        std::printf("PASS: commit()'s scanAll reaches every staged write\n");
        return 0;
    }
    std::printf(
        "FAIL: %d of %d staged writes unreachable by commit()'s scanAll -> never "
        "committed, permanently lost (AUDIT B14; chain severed per B13)\n",
        N - promoted, N);
    return 1;
}
