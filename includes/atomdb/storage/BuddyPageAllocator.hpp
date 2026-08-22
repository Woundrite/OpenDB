#ifndef ATOMDB_BUDDY_PAGE_ALLOCATOR_HPP
#define ATOMDB_BUDDY_PAGE_ALLOCATOR_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "atomdb/contracts/IPageAllocator.hpp"

namespace atomdb {

// BuddyPageAllocator: binary-buddy page allocator.
//
// Slab classes are powers of two: class k holds free runs of length 2^k pages.
// Class 0 = 1 page, class 1 = 2 pages, ..., class MAX_CLASS = 2^MAX_CLASS pages.
//
// Allocation:
//   1. Determine target class k = ceil_log2(n).
//   2. Walk k, k+1, ..., MAX_CLASS looking for any free run in freeBuddy_[k'].
//      If found at class k' > k, split it downward until we get a class-k run.
//   3. If no free run found in any class, call extendFile(n) and use the
//      returned range as a single n-page allocation (no splitting needed;
//      extendFile returns exactly n pages).
//   4. Mark allocated_[start..start+n-1] = true.
//
// Free:
//   1. Mark allocated_[start..start+n-1] = false.
//   2. Push the run onto freeBuddy_[k] where k = ceil_log2(n).
//   3. Coalesce upward: if the buddy of the run at class k is also free,
//      remove both runs from freeBuddy_[k] and push the coalesced run onto
//      freeBuddy_[k+1]. Repeat until no coalesce possible or k == MAX_CLASS.
//
// Coalescing requires knowing which pages are free, hence the in-memory
// llocated_ bitmap. The bitmap is rebuilt on open() from the on-disk
// freeHead chain (v1 format preserved).
//
// On-disk state (preserved across open/close):
//   - The singly-linked freeHead chain in page 0 offset 12 (v1 layout) is
//     REWRITTEN on every freePages() so a v1 reader still sees a valid
//     free-list chain if it opens the file. The v2 header bytes are
//     diagnostic-only (freeRunCount + maxClass).
//
// Concurrency: all public methods take mu_ (matches LockManager /
// TransactionManager style of "one mutex per stateful component").
class BuddyPageAllocator final : public IPageAllocator {
public:
    // 8 classes = 1, 2, 4, 8, 16, 32, 64, 128, 256 pages. 256 pages = 1 MiB.
    static constexpr std::size_t MAX_CLASS = 8;

    BuddyPageAllocator() = default;

    // IPageAllocator
    PageId allocatePages(
        std::size_t n,
        std::function<PageId(std::size_t)> extendFile) override;
    void freePages(PageId start, std::size_t n) override;
    std::size_t freePageCount() const override;
    std::size_t freeRunCount() const override;
    void onFileExtended(PageId startPageId, std::size_t n) override;
    void serializeHeader(std::uint8_t out[16]) const override;
    void loadHeader(const std::uint8_t in[16], PageId pageCount) override;

    // Test-only: max slab class currently in use (deepest class with any runs).
    std::size_t maxObservedClass() const;

private:
    mutable std::mutex mu_;
    std::array<std::vector<PageId>, MAX_CLASS + 1> freeBuddy_; // class k -> start pages of runs
    std::vector<bool> allocated_; // bit per page; size tracks pageCount_
    PageId pageCount_ = 0;        // mirror of Pager::header_.pageCount

    static std::size_t classFor(std::size_t n);
    static PageId buddyOf(PageId page, std::size_t k);
    static bool isPowerOfTwo(std::size_t n);

    void ensureBitmapSize(PageId pageCount);

    // Unlocked variant of freeRunCount(), for internal callers (e.g.
    // serializeHeader()) that already hold mu_. std::mutex is not
    // recursive, so re-taking mu_ from a method that already holds it
    // deadlocks the calling thread rather than erroring.
    std::size_t freeRunCountUnlocked() const;
};

} // namespace atomdb

#endif // ATOMDB_BUDDY_PAGE_ALLOCATOR_HPP
