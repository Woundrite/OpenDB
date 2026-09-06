#ifndef OPENDB_BTREE_HPP
#define OPENDB_BTREE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "opendb/storage/Pager.hpp"
#include "opendb/types/Schema.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/TxnId.hpp"
#include "opendb/types/Value.hpp"

namespace opendb {

// MVCC B+Tree for LocalFileStorageEngine.
//
// Key design: composite key (user_key, commitSeq DESC).
// - commitSeq = 0 means staged (uncommitted), visible only to owning txn.
// - commitSeq > 0 means committed, visible to txns with visibleSeq >= commitSeq.
// - Tombstone flag marks deleted rows.
// - For a given user_key, versions are stored in descending commitSeq order
//   so the newest visible version is found first during scan.
//
// Leaf entry layout (packed):
//   user_key (Value, variable via toBytes)
//   commitSeq (uint64_t, 8 bytes)
//   txnId   (uint64_t, 8 bytes)  -- owning txn for staged entries
//   tombstone (bool, 1 byte)
//   payload (Tuple, variable via toBytes)  -- empty for tombstones
//
// Internal node entry layout:
//   user_key (Value)
//   child_page_id (Pager::PageId, 4 bytes)

class BTree {
public:
    // Visible sequence number passed by EngineLoop at commit time.
    using VisibleSeq = std::uint64_t;

    // Result of a point lookup.
    struct LookupResult {
        bool found = false;
        std::uint64_t commitSeq = 0;    // of the version found
        std::uint64_t txnId = 0;        // owning txn if commitSeq == 0
        bool tombstone = false;
        Tuple payload;
    };

    // Callback for scanning. Return false to stop iteration.
    using ScanCallback = std::function<bool(const Value& key, const LookupResult& result)>;

    BTree(Pager& pager, std::uint32_t root_page_id = 0);

    // Initialize a new empty tree (allocates root leaf page).
    Pager::PageId createNew();

    // Get the root page id (persisted in schema/meta).
    Pager::PageId rootPageId() const noexcept { return root_page_id_; }

    // Phase 6.4: walk every page in this BTree and push it onto the Pager's
    // free list. Called by LocalFileStorageProvider::dropTable so the pages
    // can be reused by subsequent tables instead of leaking forever.
    // After this returns, the BTree must not be used (root_page_id_ is left
    // at its old value; the caller is expected to destroy the BTree).
    void freeAllPages();

    // Point lookup: find newest version of `key` visible to `txn` at `visible_seq`.
    // If `txn` has staged a version (commitSeq == 0 && txnId == txn), it wins.
    // Otherwise, return newest committed version with commitSeq <= visible_seq.
    LookupResult get(const Value& key, TxnId txn, VisibleSeq visible_seq);

    // Insert a new version for `key`. If commit_seq == 0, this is a staged write
    // (txnId must be the owner). If commit_seq > 0, this is a committed version.
    // Returns false if a version with exactly (key, commitSeq, txnId) already exists.
    bool put(const Value& key,
             std::uint64_t commit_seq,
             std::uint64_t txn_id,
             bool tombstone,
             const Tuple& payload);

    // Scan all keys in order, invoking `cb` for each visible version.
    // Only the newest visible version per key is reported.
    void scan(TxnId txn, VisibleSeq visible_seq, ScanCallback cb);

    // Walk every (key, commitSeq, txnId, tombstone, payload) entry in storage
    // order (key ASC, commitSeq DESC per key group). No visibility filtering;
    // useful for commit-time staging collection.
    struct RawEntry {
        Value key;
        std::uint64_t commitSeq;
        std::uint64_t txnId;
        bool tombstone;
        Tuple payload;
    };
    using AllEntriesCallback = std::function<bool(const RawEntry& entry)>;
    void scanAll(AllEntriesCallback cb);

    // Persist any dirty internal state (no-op for now; all writes go through pager immediately).
    void flush();

    // Allow BTree to call Pager::allocatePage.
    friend class LocalFileStorageEngine;

private:
    // Node type discriminant.
    enum class NodeType : std::uint8_t { Internal = 0, Leaf = 1 };

    // ---- on-disk node layout -----------------------------------------------
    // Both internal and leaf nodes share a common header (after pager's 8-byte CRC):
    //   NodeType (1 byte)
    //   uint16_t num_entries
    //   uint32_t right_sibling (PageId, 0 if none)  -- for leaf-level linked list
    // Then entries follow.

    struct NodeHeader {
        NodeType type;
        std::uint16_t num_entries;
        Pager::PageId right_sibling;
    };

    // ---- in-memory entry representations -----------------------------------
    struct InternalEntry {
        Value key;
        Pager::PageId child;
    };

    struct LeafEntry {
        Value key;
        std::uint64_t commitSeq;
        std::uint64_t txnId;
        bool tombstone;
        Tuple payload;
    };

    // ---- helpers -----------------------------------------------------------
    static constexpr std::size_t NODE_HEADER_SIZE = 8; // padded for alignment

    // Load node from pager page.
    void loadNode(Pager::PageId id, NodeHeader& hdr,
                  std::vector<InternalEntry>& internal_out,
                  std::vector<LeafEntry>& leaf_out);

    // Save node to pager page.
    void saveNode(Pager::PageId id, const NodeHeader& hdr,
                  const std::vector<InternalEntry>& internal,
                  const std::vector<LeafEntry>& leaf);

    // Find leaf page containing `key` (or where it would be inserted).
    // Returns the leaf page id.
    Pager::PageId findLeaf(const Value& key);

    // Insert into leaf; split if full. Returns true if split occurred and new
    // right sibling page id is set in `new_right` and promoted key in `promoted_key`.
    bool insertIntoLeaf(Pager::PageId leaf_id, const LeafEntry& entry,
                        Pager::PageId& new_right, Value& promoted_key);

    // Insert into internal node; split if full.
    bool insertIntoInternal(Pager::PageId internal_id, const Value& key, Pager::PageId child,
                            Pager::PageId& new_right, Value& promoted_key);

    // Called after a split to insert the promoted key into the parent.
    void insertIntoParent(Pager::PageId left_child, const Value& key, Pager::PageId right_child);

    // Allocate a new page and initialize as leaf or internal.
    Pager::PageId newPage(NodeType type);

    // Split leaf page.
    void splitLeaf(Pager::PageId leaf_id, const std::vector<LeafEntry>& entries,
                   Pager::PageId& new_right, Value& promoted_key);

    // Split internal page.
    void splitInternal(Pager::PageId internal_id, const std::vector<InternalEntry>& entries,
                       Pager::PageId& new_right, Value& promoted_key);

    // Encode/decode Value to/from byte vector (uses Value::toBytes/fromBytes).
    // helpers: serialize one column's name + Value
    // (Tuple/value wire format is in encodeTuple / decodeTuple below).
    // Encode/decode Tuple to/from byte vector.
    static std::vector<std::uint8_t> encodeTuple(const Tuple& t);
    static Tuple decodeTuple(const std::vector<std::uint8_t>& data, std::size_t& offset);

    Pager& pager_;
    Pager::PageId root_page_id_ = 0;
};

} // namespace opendb

#endif // OPENDB_BTREE_HPP