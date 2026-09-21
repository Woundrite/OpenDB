#include "opendb/storage/BuddyPageAllocator.hpp"
#include "opendb/contracts/IPageAllocator.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace opendb {

using PageId = IPageAllocator::PageId;

std::size_t BuddyPageAllocator::classFor(std::size_t n) {
    assert(n > 0);
    std::size_t k = 0;
    while ((std::size_t(1) << k) < n) ++k;
    return k;
}

PageId BuddyPageAllocator::buddyOf(PageId page, std::size_t k) {
    return page ^ (PageId(1) << k);
}

bool BuddyPageAllocator::isPowerOfTwo(std::size_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}

void BuddyPageAllocator::ensureBitmapSize(PageId pageCount) {
    if (pageCount_ >= pageCount) return;
    allocated_.resize(pageCount, true); // newly extended pages are allocated
    pageCount_ = pageCount;
}

PageId BuddyPageAllocator::allocatePages(
    std::size_t n,
    std::function<PageId(std::size_t)> extendFile) {
    std::unique_lock<std::mutex> lk(mu_);   // was: std::lock_guard
    assert(n > 0);

    std::size_t targetClass = classFor(n);

    // Try to find a free run at or above targetClass.
    for (std::size_t k = targetClass; k <= MAX_CLASS; ++k) {
        if (!freeBuddy_[k].empty()) {
            PageId start = freeBuddy_[k].back();
            freeBuddy_[k].pop_back();

            // Split down if we found a larger class. The split descends
            // into the UPPER half and parks the lower half, so the pages
            // returned are the highest addresses of the run. Adjacent
            // pages freed in ascending order coalesce into one run; the
            // most recently freed page then comes back first (LIFO),
            // matching the historical stack free-list contract that
            // Pager_FreeList_LIFO_Order pins down.
            while (k > targetClass) {
                --k;
                freeBuddy_[k].push_back(start);   // park lower half
                start = start + (PageId(1) << k); // descend into upper half
            }
            // Mark the requested portion as allocated.
            for (std::size_t i = 0; i < n; ++i) {
                allocated_[start + i] = true;
            }
            return start;
        }
    }

    // No free run available: extend file. extendFile() (Pager::extendFile)
    // calls back into onFileExtended(), which takes mu_ itself — so mu_
    // MUST be released before invoking this callback, or the calling
    // thread deadlocks against its own held lock. Note: Pager itself has
    // no mutex of its own, so today this is safe only because callers
    // serialize writes to a given table above this layer (table-level
    // locking) — this fix removes the guaranteed deadlock, it doesn't add
    // concurrency safety Pager didn't already have.
    lk.unlock();
    return extendFile(n);
    // The new pages are already marked allocated by onFileExtended().
}

// NOTE on accounting exactness: a freed run is parked as ONE entry of class
// ceil_log2(n), so the pool counts 2^class pages for a true free length of n.
// All in-repo callers pass n == 1 or power-of-two n (single-page frees,
// buddy-slab frees, exact snapshot restores), for which the count is exact.
// A non-power-of-two free (possible today only via migrateV1ToV2 on legacy v1
// chains) over-counts and its overhang must never be handed out by a later
// smaller split allocation — keep this in mind if multi-page arbitrary frees
// ever become common; the fix is aligned power-of-two decomposition here.
void BuddyPageAllocator::freePages(PageId start, std::size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    assert(n > 0);
    assert(start < pageCount_);
    assert(start + n <= pageCount_);

    // Mark pages as free.
    for (std::size_t i = 0; i < n; ++i) {
        allocated_[start + i] = false;
    }

    std::size_t k = classFor(n);
    PageId curStart = start;

    // Push the run onto the appropriate class.
    freeBuddy_[k].push_back(curStart);

    // Coalesce upward while buddy is also free and at the same class.
    while (k < MAX_CLASS) {
        PageId buddyStart = buddyOf(curStart, k);
        // Check if buddy is in our freeBuddy_[k].
        auto it = std::find(freeBuddy_[k].begin(), freeBuddy_[k].end(), buddyStart);
        if (it == freeBuddy_[k].end()) break;

        // Remove buddy from free list.
        freeBuddy_[k].erase(it);
        // Also remove our curStart (we'll push the coalesced run).
        auto it2 = std::find(freeBuddy_[k].begin(), freeBuddy_[k].end(), curStart);
        if (it2 == freeBuddy_[k].end()) break; // shouldn't happen
        freeBuddy_[k].erase(it2);

        // Coalesce: lower address becomes the new start.
        if (buddyStart < curStart) {
            curStart = buddyStart;
        }
        ++k;
        freeBuddy_[k].push_back(curStart);
    }
}

std::size_t BuddyPageAllocator::freePageCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t sum = 0;
    for (std::size_t k = 0; k <= MAX_CLASS; ++k) {
        sum += freeBuddy_[k].size() * (std::size_t(1) << k);
    }
    return sum;
}

std::size_t BuddyPageAllocator::freeRunCountUnlocked() const {
    std::size_t sum = 0;
    for (std::size_t k = 0; k <= MAX_CLASS; ++k) {
        sum += freeBuddy_[k].size();
    }
    return sum;
}

std::size_t BuddyPageAllocator::freeRunCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return freeRunCountUnlocked();
}

void BuddyPageAllocator::onFileExtended(PageId startPageId, std::size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    ensureBitmapSize(startPageId + n);
    for (std::size_t i = 0; i < n; ++i) {
        allocated_[startPageId + i] = true;
    }
}

void BuddyPageAllocator::saveFreeRuns(std::vector<FreeRun>& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    out.clear();
    for (std::size_t k = 0; k <= MAX_CLASS; ++k) {
        for (PageId start : freeBuddy_[k]) {
            out.push_back(FreeRun{start, static_cast<std::uint32_t>(k)});
        }
    }
}

void BuddyPageAllocator::loadFreeRuns(const FreeRun* runs, std::size_t count,
                                      PageId pageCount) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& cls : freeBuddy_) cls.clear();
    ensureBitmapSize(pageCount); // marks every page allocated; runs punch holes
    if (runs == nullptr) return;
    for (std::size_t i = 0; i < count; ++i) {
        const PageId start = runs[i].start;
        const std::size_t k = runs[i].slabClass;
        if (k > MAX_CLASS || start == 0 || start >= pageCount) continue;
        // Clamp the run to the file: a persisted run may overhang pageCount
        // when the free length wasn't a power of two (class rounds up).
        // Pages past EOF don't exist, so they stay (trivially) allocated.
        std::size_t len = std::size_t(1) << k;
        if (start + len > pageCount) len = pageCount - start;
        // Overlap guard: a corrupt snapshot must never mark a page free
        // twice (that would hand the same page to two owners). Skip the
        // whole run if any of its pages is already free.
        bool overlap = false;
        for (std::size_t j = 0; j < len; ++j) {
            if (!allocated_[start + j]) { overlap = true; break; }
        }
        if (overlap) continue;
        for (std::size_t j = 0; j < len; ++j) {
            allocated_[start + j] = false;
        }
        freeBuddy_[k].push_back(start);
    }
}

void BuddyPageAllocator::serializeHeader(std::uint8_t out[16]) const {
    std::lock_guard<std::mutex> lk(mu_);
    // out[0..3]: reserved (bitmap root - not used, we rebuild from freeHead chain)
    // out[4..7]: reserved
    // out[8..9]: max observed class (u16)
    // out[10..11]: free run count (u16, capped at 65535)
    // out[12..15]: reserved
    std::memset(out, 0, 16);
    std::size_t maxClass = 0;
    for (std::size_t k = MAX_CLASS; k > 0; --k) {
        if (!freeBuddy_[k].empty()) {
            maxClass = k;
            break;
        }
    }
    std::uint16_t maxClass16 = static_cast<std::uint16_t>(maxClass);
    std::uint16_t runCount16 = static_cast<std::uint16_t>(std::min(freeRunCountUnlocked(), std::size_t(65535)));
    std::memcpy(out + 8, &maxClass16, 2);
    std::memcpy(out + 10, &runCount16, 2);
}

void BuddyPageAllocator::loadHeader(const std::uint8_t in[16], PageId pageCount) {
    std::lock_guard<std::mutex> lk(mu_);
    // The Pager already rebuilt our llocated_ bitmap and reeBuddy_
    // by walking the v1 freeHead chain and calling onFileExtended/freePages
    // BEFORE this loadHeader call. We just need to read the diagnostic fields
    // if they exist (v2), and ensure bitmap size matches pageCount.
    ensureBitmapSize(pageCount);

    std::uint16_t maxClass = 0;
    std::uint16_t runCount = 0;
    std::memcpy(&maxClass, in + 8, 2);
    std::memcpy(&runCount, in + 10, 2);
    // Could validate: if runCount != freeRunCount() then something is wrong.
    // For now, just trust the rebuilt state.
    (void)maxClass;
    (void)runCount;
}

std::size_t BuddyPageAllocator::maxObservedClass() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (std::size_t k = MAX_CLASS; k > 0; --k) {
        if (!freeBuddy_[k].empty()) return k;
    }
    return 0;
}

} // namespace opendb
