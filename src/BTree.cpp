#include "atomdb/storage/BTree.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace atomdb {

// ---- BTree constructor ------------------------------------------------------

BTree::BTree(Pager& pager, std::uint32_t root_page_id)
    : pager_(pager), root_page_id_(root_page_id) {}

Pager::PageId BTree::createNew() {
    root_page_id_ = newPage(NodeType::Leaf);
    return root_page_id_;
}

// ---- Value encode/decode (delegates to Value::toBytes/fromBytes) ------------
// ponytail: BTree::encodeKey/decodeKey were 1-line wrappers around Value's own
// toBytes/fromBytes; removed — callers now use those directly.

// ---- Tuple encode/decode ----------------------------------------------------
// ponytail: keep one (de)serializer; no second format elsewhere.

std::vector<std::uint8_t> BTree::encodeTuple(const Tuple& t) {
    std::vector<std::uint8_t> out;
    auto& cols = t.columns();
    std::uint32_t count = static_cast<std::uint32_t>(cols.size());
    out.insert(out.end(),
               reinterpret_cast<const std::uint8_t*>(&count),
               reinterpret_cast<const std::uint8_t*>(&count) + 4);
    for (const auto& cv : cols) {
        std::uint32_t nlen = static_cast<std::uint32_t>(cv.name.size());
        out.insert(out.end(),
                   reinterpret_cast<const std::uint8_t*>(&nlen),
                   reinterpret_cast<const std::uint8_t*>(&nlen) + 4);
        out.insert(out.end(), cv.name.begin(), cv.name.end());
        auto vbytes = cv.value.toBytes();
        out.insert(out.end(), vbytes.begin(), vbytes.end());
    }
    return out;
}

Tuple BTree::decodeTuple(const std::vector<std::uint8_t>& data, std::size_t& offset) {
    Tuple t;
    if (offset + 4 > data.size()) return t;
    std::uint32_t count = 0;
    std::memcpy(&count, &data[offset], 4);
    offset += 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + 4 > data.size()) break;
        std::uint32_t nlen = 0;
        std::memcpy(&nlen, &data[offset], 4);
        offset += 4;
        if (offset + nlen > data.size()) break;
        std::string name(reinterpret_cast<const char*>(&data[offset]), nlen);
        offset += nlen;
        // ponytail: inlined decodeKey — it was 1 line wrapping Value::fromBytes.
        auto v_it = data.begin() + static_cast<std::ptrdiff_t>(offset);
        Value v = Value::fromBytes(v_it);
        offset = static_cast<std::size_t>(std::distance(data.begin(), v_it));
        t.set(name, std::move(v));
    }
    return t;
}

// ---- Node load / save -------------------------------------------------------

// ponytail: inlined one-liner wrapper — replace 5 call sites, see below.

void BTree::loadNode(Pager::PageId id, NodeHeader& hdr,
                     std::vector<InternalEntry>& internal_out,
                     std::vector<LeafEntry>& leaf_out) {
    std::vector<std::uint8_t> page;
    if (!pager_.readPage(id, page)) {
        throw std::runtime_error("BTree::loadNode: failed to read page " + std::to_string(id));
    }
    const std::uint8_t* data = page.data();
    std::size_t offset = 0;

    hdr.type = static_cast<NodeType>(data[offset++]);
    std::memcpy(&hdr.num_entries, data + offset, 2);
    offset += 2;
    std::memcpy(&hdr.right_sibling, data + offset, 4);
    offset += 4;
    // Align to 8-byte boundary (NODE_HEADER_SIZE = 8).
    offset = (offset + 7) & ~static_cast<std::size_t>(7);

    internal_out.clear();
    leaf_out.clear();

    if (hdr.type == NodeType::Internal) {
        internal_out.reserve(hdr.num_entries);
        for (std::uint16_t i = 0; i < hdr.num_entries; ++i) {
            InternalEntry e;
            auto v_it = page.cbegin() + static_cast<std::ptrdiff_t>(offset);
            e.key = Value::fromBytes(v_it);
            offset = static_cast<std::size_t>(std::distance(page.cbegin(), v_it));
            std::memcpy(&e.child, data + offset, 4);
            offset += 4;
            internal_out.push_back(std::move(e));
        }
    } else {
        leaf_out.reserve(hdr.num_entries);
        for (std::uint16_t i = 0; i < hdr.num_entries; ++i) {
            LeafEntry e;
            auto v_it = page.cbegin() + static_cast<std::ptrdiff_t>(offset);
            e.key = Value::fromBytes(v_it);
            offset = static_cast<std::size_t>(std::distance(page.cbegin(), v_it));
            std::memcpy(&e.commitSeq, data + offset, 8);
            offset += 8;
            std::memcpy(&e.txnId, data + offset, 8);
            offset += 8;
            e.tombstone = (data[offset++] != 0);
            // Tuple is length-prefixed.
            std::uint32_t tlen = 0;
            std::memcpy(&tlen, data + offset, 4);
            offset += 4;
            std::size_t tuple_end = offset + tlen;
            // ponytail: clamp malformed length so we don't spill into the next entry.
            if (tuple_end > page.size()) tuple_end = page.size();
            e.payload = decodeTuple(page, offset);
            offset = tuple_end;
            leaf_out.push_back(std::move(e));
        }
    }
}

void BTree::saveNode(Pager::PageId id, const NodeHeader& hdr,
                     const std::vector<InternalEntry>& internal,
                     const std::vector<LeafEntry>& leaf) {
    std::vector<std::uint8_t> page(Pager::DATA_PAGE_PAYLOAD_SIZE, 0);
    std::uint8_t* data = page.data();
    std::size_t offset = 0;

    data[offset++] = static_cast<std::uint8_t>(hdr.type);
    std::memcpy(data + offset, &hdr.num_entries, 2);
    offset += 2;
    std::memcpy(data + offset, &hdr.right_sibling, 4);
    offset += 4;
    offset = (offset + 7) & ~static_cast<std::size_t>(7); // align to 8

    if (hdr.type == NodeType::Internal) {
        for (const auto& e : internal) {
            auto key_bytes = e.key.toBytes();
            std::memcpy(data + offset, key_bytes.data(), key_bytes.size());
            offset += key_bytes.size();
            std::memcpy(data + offset, &e.child, 4);
            offset += 4;
        }
    } else {
        for (const auto& e : leaf) {
            auto key_bytes = e.key.toBytes();
            std::memcpy(data + offset, key_bytes.data(), key_bytes.size());
            offset += key_bytes.size();
            std::memcpy(data + offset, &e.commitSeq, 8);
            offset += 8;
            std::memcpy(data + offset, &e.txnId, 8);
            offset += 8;
            data[offset++] = e.tombstone ? 1 : 0;
            auto payload_bytes = encodeTuple(e.payload);
            std::uint32_t plen = static_cast<std::uint32_t>(payload_bytes.size());
            std::memcpy(data + offset, &plen, 4);
            offset += 4;
            if (offset + payload_bytes.size() > Pager::DATA_PAGE_PAYLOAD_SIZE) {
                throw std::runtime_error("BTree::saveNode: leaf page byte overflow (offset=" + std::to_string(offset) + ", payload=" + std::to_string(payload_bytes.size()) + ", cap=" + std::to_string(Pager::DATA_PAGE_PAYLOAD_SIZE) + ")");
            }
            std::memcpy(data + offset, payload_bytes.data(), payload_bytes.size());
            offset += payload_bytes.size();
        }
    }

    pager_.writePage(id, page);
}

// (bounds check moved into saveNode loop above via asserts; kept separately for clarity)
// ---- findLeaf ---------------------------------------------------------------

Pager::PageId BTree::findLeaf(const Value& key) {
    Pager::PageId cur = root_page_id_;
    while (true) {
        NodeHeader hdr;
        std::vector<InternalEntry> internal;
        std::vector<LeafEntry> leaf;
        loadNode(cur, hdr, internal, leaf);
        if (hdr.type == NodeType::Leaf) {
            return cur;
        }
        // Internal: find rightmost entry where key >= entry.key.
        Pager::PageId next = 0;
        for (const auto& e : internal) {
            if (key >= e.key) {
                next = e.child;
            } else {
                break;
            }
        }
        if (next == 0) next = internal.front().child;
        cur = next;
    }
}

// ---- get --------------------------------------------------------------------

BTree::LookupResult BTree::get(const Value& key, TxnId txn, VisibleSeq visible_seq) {
    Pager::PageId leaf_id = findLeaf(key);
    NodeHeader hdr;
    std::vector<InternalEntry> internal;
    std::vector<LeafEntry> leaf;
    loadNode(leaf_id, hdr, internal, leaf);

    LookupResult result;
    // Entries for this key are contiguous (sorted by key, then commitSeq DESC).
    for (const auto& e : leaf) {
        if (e.key != key) continue;
        // Check visibility.
        if (e.commitSeq == 0) {
            // Staged version: visible only to owning txn.
            if (e.txnId == txn.value()) {
                result.found = true;
                result.commitSeq = 0;
                result.txnId = e.txnId;
                result.tombstone = e.tombstone;
                result.payload = e.payload;
                return result;
            }
        } else if (e.commitSeq <= visible_seq) {
            // Committed version visible.
            result.found = true;
            result.commitSeq = e.commitSeq;
            result.txnId = e.txnId;
            result.tombstone = e.tombstone;
            result.payload = e.payload;
            return result;
        }
        // Not visible, continue to older versions (they're in DESC order).
    }
    return result; // found = false
}

// ---- put --------------------------------------------------------------------

bool BTree::put(const Value& key,
                std::uint64_t commit_seq,
                std::uint64_t txn_id,
                bool tombstone,
                const Tuple& payload) {
    Pager::PageId leaf_id = findLeaf(key);
    NodeHeader hdr;
    std::vector<InternalEntry> internal;
    std::vector<LeafEntry> leaf;
    loadNode(leaf_id, hdr, internal, leaf);

    // Check for duplicate (key, commit_seq, txn_id) — should not happen.
    for (const auto& e : leaf) {
        if (e.key == key && e.commitSeq == commit_seq && e.txnId == txn_id) {
            return false;
        }
    }

    // Find insert position: keys sorted ASC, commitSeq DESC.
    // So newer versions come BEFORE older ones for same key.
    auto it = leaf.begin();
    while (it != leaf.end()) {
        if (key < it->key) break;
        if (key == it->key && commit_seq > it->commitSeq) break;
        if (key == it->key && commit_seq == it->commitSeq && txn_id < it->txnId) break;
        ++it;
    }

    LeafEntry new_entry{key, commit_seq, txn_id, tombstone, payload};
    leaf.insert(it, std::move(new_entry));
    ++hdr.num_entries;

    // Check if leaf overflowed. We split if either the entry count OR the
    // serialized byte size exceeds the page capacity; the latter would otherwise
    // smash the heap. ponytail: count limit + byte limit, ~5 lines.
    constexpr std::uint16_t MAX_LEAF_ENTRIES = 64;
    auto approxBytes = [&] {
        // Sum of encoded bytes for every entry; payload bytes dominate so we
        // measure by re-running the encoder (cheap; one leaf per put).
        std::size_t n = 0;
        for (const auto& e : leaf) {
            n += e.key.toBytes().size() + 8 /*commitSeq*/ + 8 /*txnId*/ + 1 /*tomb*/ + 4 /*tlen*/ + encodeTuple(e.payload).size();
        }
        return n;
    };
    if (hdr.num_entries > MAX_LEAF_ENTRIES || approxBytes() > Pager::DATA_PAGE_PAYLOAD_SIZE - 64) {
        Pager::PageId new_right;
        Value promoted_key;
        splitLeaf(leaf_id, leaf, new_right, promoted_key);

        // Insert promoted key into parent (or create new root).
        insertIntoParent(leaf_id, promoted_key, new_right);
        return true;
    }

    saveNode(leaf_id, hdr, internal, leaf);
    return true;
}

// ---- insertIntoParent -------------------------------------------------------

void BTree::insertIntoParent(Pager::PageId left_child, const Value& key, Pager::PageId right_child) {
    // Iterative split-propagation. We walk up the tree level-by-level instead of
    // recursing, so stack usage is bounded regardless of tree height.
    Pager::PageId cur_left = left_child;
    Value cur_key = key;
    Pager::PageId cur_right = right_child;

    while (true) {
        // Special case: if root is currently a leaf, create a new internal root.
        NodeHeader root_hdr;
        std::vector<InternalEntry> root_internal;
        std::vector<LeafEntry> root_leaf;
        loadNode(root_page_id_, root_hdr, root_internal, root_leaf);

        if (root_hdr.type == NodeType::Leaf) {
            Pager::PageId old_root = root_page_id_;
            Pager::PageId new_root = newPage(NodeType::Internal);
            NodeHeader new_hdr{NodeType::Internal, 2, 0};
            std::vector<InternalEntry> new_internal;
            new_internal.push_back({Value::null(), old_root});  // leftmost = -inf
            new_internal.push_back({cur_key, cur_right});
            saveNode(new_root, new_hdr, new_internal, {});
            root_page_id_ = new_root;
            return;
        }

        // Walk from root to find the parent of cur_left.
        Pager::PageId parent = 0;
        std::vector<Pager::PageId> stack;
        stack.push_back(root_page_id_);

        while (!stack.empty()) {
            Pager::PageId cur = stack.back();
            stack.pop_back();

            NodeHeader hdr;
            std::vector<InternalEntry> internal;
            std::vector<LeafEntry> leaf;
            loadNode(cur, hdr, internal, leaf);

            if (hdr.type == NodeType::Internal) {
                for (const auto& e : internal) {
                    if (e.child == cur_left) {
                        parent = cur;
                        break;
                    }
                }
                if (parent != 0) break;
                for (const auto& e : internal) {
                    stack.push_back(e.child);
                }
            }
        }

        if (parent == 0) {
            throw std::runtime_error("insertIntoParent: parent of left_child not found");
        }

        // Insert (cur_key, cur_right) into parent.
        NodeHeader phdr;
        std::vector<InternalEntry> pinternal;
        std::vector<LeafEntry> pleaf;
        loadNode(parent, phdr, pinternal, pleaf);

        auto it = pinternal.begin();
        while (it != pinternal.end() && cur_key >= it->key) ++it;
        pinternal.insert(it, {cur_key, cur_right});
        ++phdr.num_entries;

        constexpr std::uint16_t MAX_INTERNAL_ENTRIES = 128;
        if (phdr.num_entries <= MAX_INTERNAL_ENTRIES) {
            saveNode(parent, phdr, pinternal, pleaf);
            return;
        }

        // Internal overflow: split and promote.
        Pager::PageId new_right;
        Value promoted_key;
        splitInternal(parent, pinternal, new_right, promoted_key);
        // splitInternal writes both (parent = left, new_right = right) halves.
        // Loop again with (parent, promoted_key, new_right) to insert the
        // separator into the parent's parent.
        cur_left = parent;
        cur_key = promoted_key;
        cur_right = new_right;
    }
}
// ---- splitLeaf --------------------------------------------------------------

void BTree::splitLeaf(Pager::PageId leaf_id, const std::vector<LeafEntry>& entries,
                      Pager::PageId& new_right, Value& promoted_key) {
    std::size_t mid = entries.size() / 2;

    std::vector<LeafEntry> left_entries(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(mid));
    std::vector<LeafEntry> right_entries(entries.begin() + static_cast<std::ptrdiff_t>(mid), entries.end());

    promoted_key = right_entries.front().key; // first key of right page

    // Allocate right page first (needed for left's right_sibling pointer).
    new_right = newPage(NodeType::Leaf);

    // Rewrite left page with updated right_sibling.
    NodeHeader lhdr{NodeType::Leaf, static_cast<std::uint16_t>(left_entries.size()), new_right};
    saveNode(leaf_id, lhdr, {}, left_entries);

    // Write right page.
    NodeHeader rhdr{NodeType::Leaf, static_cast<std::uint16_t>(right_entries.size()), 0};
    saveNode(new_right, rhdr, {}, right_entries);
}

// ---- splitInternal ----------------------------------------------------------

void BTree::splitInternal(Pager::PageId internal_id, const std::vector<InternalEntry>& entries,
                          Pager::PageId& new_right, Value& promoted_key) {
    std::size_t mid = entries.size() / 2;

    std::vector<InternalEntry> left_entries(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(mid));
    std::vector<InternalEntry> right_entries(entries.begin() + static_cast<std::ptrdiff_t>(mid), entries.end());

    promoted_key = right_entries.front().key;

    new_right = newPage(NodeType::Internal);

    NodeHeader lhdr{NodeType::Internal, static_cast<std::uint16_t>(left_entries.size()), 0};
    saveNode(internal_id, lhdr, left_entries, {});

    NodeHeader rhdr{NodeType::Internal, static_cast<std::uint16_t>(right_entries.size()), 0};
    saveNode(new_right, rhdr, right_entries, {});
}

// ---- newPage ----------------------------------------------------------------

Pager::PageId BTree::newPage(NodeType type) {
    Pager::PageId id = pager_.allocatePage();
    // Initialize empty page of given type.
    NodeHeader hdr{type, 0, 0};
    std::vector<InternalEntry> empty_internal;
    std::vector<LeafEntry> empty_leaf;
    saveNode(id, hdr, empty_internal, empty_leaf);
    return id;
}

// ---- scan -------------------------------------------------------------------

void BTree::scan(TxnId txn, VisibleSeq visible_seq, ScanCallback cb) {
    // Walk leaf linked list from leftmost leaf.
    Pager::PageId cur = root_page_id_;
    // Descend to leftmost leaf.
    while (true) {
        NodeHeader hdr;
        std::vector<InternalEntry> internal;
        std::vector<LeafEntry> leaf;
        loadNode(cur, hdr, internal, leaf);
        if (hdr.type == NodeType::Leaf) break;
        cur = internal.front().child;
    }

    // Iterate leaves via right_sibling chain.
    while (cur != 0) {
        NodeHeader hdr;
        std::vector<InternalEntry> internal;
        std::vector<LeafEntry> leaf;
        loadNode(cur, hdr, internal, leaf);

        // For each key, find newest visible version.
        std::size_t i = 0;
        while (i < leaf.size()) {
            const Value& key = leaf[i].key;
            // Collect all versions of this key (contiguous, DESC commitSeq).
            std::size_t j = i;
            const LeafEntry* visible = nullptr;
            while (j < leaf.size() && leaf[j].key == key) {
                const auto& e = leaf[j];
                if (e.commitSeq == 0) {
                    if (e.txnId == txn.value()) {
                        visible = &e;
                        break;
                    }
                } else if (e.commitSeq <= visible_seq) {
                    visible = &e;
                    break;
                }
                ++j;
            }
            if (visible) {
                LookupResult r;
                r.found = true;
                r.commitSeq = visible->commitSeq;
                r.txnId = visible->txnId;
                r.tombstone = visible->tombstone;
                r.payload = visible->payload;
                if (!cb(key, r)) return; // stop
            }
            // Advance to the FIRST entry past the current key group. We must skip
            // every entry whose key equals `key` (regardless of visibility), so we
            // never report the same key twice in one scan.
            std::size_t next = i;
            while (next < leaf.size() && leaf[next].key == key) ++next;
            i = next;
        }

        cur = hdr.right_sibling;
    }
}

// ---- scanAll ----------------------------------------------------------------

void BTree::scanAll(AllEntriesCallback cb) {
    // Walk leaf linked list from leftmost leaf.
    Pager::PageId cur = root_page_id_;
    while (true) {
        NodeHeader hdr;
        std::vector<InternalEntry> internal;
        std::vector<LeafEntry> leaf;
        loadNode(cur, hdr, internal, leaf);
        if (hdr.type == NodeType::Leaf) break;
        cur = internal.front().child;
    }
    while (cur != 0) {
        NodeHeader hdr;
        std::vector<InternalEntry> internal;
        std::vector<LeafEntry> leaf;
        loadNode(cur, hdr, internal, leaf);
        for (const auto& e : leaf) {
            RawEntry r{e.key, e.commitSeq, e.txnId, e.tombstone, e.payload};
            if (!cb(r)) return; // stop
        }
        cur = hdr.right_sibling;
    }
}

// ---- flush ------------------------------------------------------------------

void BTree::flush() {
    // All writes go through pager immediately; flush the file.
    pager_.sync();
}

// ---- freeAllPages (Phase 6.4) ---------------------------------------------

void BTree::freeAllPages() {
    // Iterative DFS over the BTree from root_page_id_. Internal nodes carry
    // their children by PageId; leaves are linked by right_sibling so the DFS
    // from the root will eventually visit every leaf without needing the
    // sibling chain. Both kinds are freed; the walk is bounded by the page
    // count of the tree.
    if (root_page_id_ == 0) return;
    std::unordered_set<Pager::PageId> visited;
    std::vector<Pager::PageId> stack;
    stack.push_back(root_page_id_);
    while (!stack.empty()) {
        Pager::PageId id = stack.back();
        stack.pop_back();
        if (id == 0) continue;
        if (!visited.insert(id).second) continue;
        NodeHeader hdr;
        std::vector<InternalEntry> internal;
        std::vector<LeafEntry> leaf;
        loadNode(id, hdr, internal, leaf);
        if (hdr.type == NodeType::Internal) {
            for (const auto& e : internal) {
                if (e.child != 0) stack.push_back(e.child);
            }
        }
        pager_.freePage(id);
    }
    root_page_id_ = 0;
}

} // namespace atomdb
