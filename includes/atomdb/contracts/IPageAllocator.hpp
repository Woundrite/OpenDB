#ifndef ATOMDB_IPAGE_ALLOCATOR_HPP
#define ATOMDB_IPAGE_ALLOCATOR_HPP

#include <cstddef>
#include <cstdint>
#include <functional>

namespace atomdb {

// IPageAllocator: the page-level allocation strategy behind the Pager.
//
// Swap implementations to trade fragmentation behavior vs. CPU/lock cost:
//   - BuddyPageAllocator:      O(1) ops, ~50% internal fragmentation on
//                              worst-case requests, simple single mutex.
//                              Best for: single writer, fixed-size pages,
//                              power-of-two growth. (Phase 6.5 default.)
//   - SlabClassPageAllocator:  O(log n) ops, ~0% internal fragmentation,
//                              coalesces on free. (Future phase.)
//   - BitmapBestFitAllocator:  O(n) ops, near-optimal fit. (Future phase.)
//
// Composition: the Pager owns an IPageAllocator by unique_ptr. The allocator
// owns its own in-memory state but NEVER touches on-disk page bytes; the
// Pager handles all read/write/sync. The allocator communicates persistence
// needs via the 16-byte header scratch buffer (serializeHeader / loadHeader)
// and via the `extendFile` callback the Pager passes to allocatePages.
//
// Threading: every method may be called from any thread. Implementations are
// responsible for their own internal synchronization. OpenDB's single-writer
// EngineLoop means contention is negligible in practice, but the contract is
// thread-safe so a multi-writer EngineLoop can drop in without changes here.
class IPageAllocator {
public:
    using PageId = std::uint32_t;
    static constexpr PageId INVALID_PAGE = 0xFFFFFFFFu;

    // Allocate `n` contiguous pages (n >= 1). Returns the first PageId of
    // the run; the next n-1 pages are also valid (and contiguous).
    //
    // `extendFile` is the callback the allocator invokes when it needs more
    // pages than the free pool contains. The Pager extends the file by `n`
    // pages, calls onFileExtended() with the new start id, and returns it.
    // The allocator must call extendFile exactly once per allocation that
    // cannot be satisfied from the existing free pool, with the exact `n`
    // it needs (no more, no less).
    virtual PageId allocatePages(
        std::size_t n,
        std::function<PageId(std::size_t)> extendFile) = 0;

    // Free a contiguous run of `n` pages starting at `start`. The allocator
    // coalesces with adjacent free runs if its strategy supports it.
    // No-op if any page in the run is already free (defensive; not asserted).
    virtual void freePages(PageId start, std::size_t n) = 0;

    // Total free pages currently in the allocator's pool (test-only
    // diagnostic; may be O(n) for some implementations).
    virtual std::size_t freePageCount() const = 0;

    // Number of distinct free runs currently parked (test-only diagnostic;
    // for the buddy allocator this is the sum of freeBuddy_[k].size()).
    virtual std::size_t freeRunCount() const = 0;

    // Notification that the file was extended by `n` pages starting at
    // `startPageId`. The allocator marks these as allocated. The Pager calls
    // this for both the extendFile callback return path AND for the
    // pageCount-bump path when a writePage extends the file directly.
    virtual void onFileExtended(PageId startPageId, std::size_t n) = 0;

    // Persistence: the allocator may need to write its own bookkeeping into
    // page 0's reserved bytes (bitmap location, max class, etc.). The Pager
    // calls `serializeHeader(buf)` to fill a 16-byte scratch buffer that
    // gets stored at offset 12 in page 0 (replacing the v1 freeHead field).
    // The Pager also calls this on every flushHeader() so the on-disk state
    // stays current even if no full close() happens.
    virtual void serializeHeader(std::uint8_t out[16]) const = 0;

    // Inverse: at open time, the Pager hands the allocator the 16 bytes
    // from page 0 offset 12 and `pageCount`. The allocator rebuilds its
    // in-memory state. For v1 files the Pager walks the singly-linked
    // freeHead chain and calls `onFileExtended()` for allocated runs and
    // `freePages()` for free runs BEFORE invoking loadHeader(), so the
    // allocator's bitmap is already in sync by the time this is called.
    virtual void loadHeader(const std::uint8_t in[16], PageId pageCount) = 0;

    virtual ~IPageAllocator() = default;
};

} // namespace atomdb

#endif // ATOMDB_IPAGE_ALLOCATOR_HPP
