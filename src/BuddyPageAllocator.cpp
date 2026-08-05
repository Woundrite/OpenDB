#include "atomdb/storage/BuddyPageAllocator.hpp"
#include "atomdb/contracts/IPageAllocator.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace atomdb {

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
    std::lock_guard<std::mutex> lk(mu_);
    assert(n > 0);

    std::size_t targetClass = classFor(n);

    // Try to find a free run at or above targetClass.
    for (std::size_t k = targetClass; k <= MAX_CLASS; ++k) {
        if (!freeBuddy_[k].empty()) {
            PageId start = freeBuddy_[k].back();
            freeBuddy_[k].pop_back();

            // Split down if we found a larger class.
            while (k > targetClass) {
                --k;
                PageId buddy = start + (PageId(1) << k); // upper half
                freeBuddy_[k].push_back(buddy);
            }
            // Mark the requested portion as allocated.
            for (std::size_t i = 0; i < n; ++i) {
                allocated_[start + i] = true;
            }
            return start;
        }
    }

    // No free run available: extend file.
    PageId start = extendFile(n);
    // The new pages are already marked allocated by onFileExtended.
    return start;
}

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

std::size_t BuddyPageAllocator::freeRunCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t sum = 0;
    for (std::size_t k = 0; k <= MAX_CLASS; ++k) {
        sum += freeBuddy_[k].size();
    }
    return sum;
}

void BuddyPageAllocator::onFileExtended(PageId startPageId, std::size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    ensureBitmapSize(startPageId + n);
    for (std::size_t i = 0; i < n; ++i) {
        allocated_[startPageId + i] = true;
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
    std::uint16_t runCount16 = static_cast<std::uint16_t>(std::min(freeRunCount(), std::size_t(65535)));
    std::memcpy(out + 8, &maxClass16, 2);
    std::memcpy(out + 10, &runCount16, 2);
}

void BuddyPageAllocator::loadHeader(const std::uint8_t in[16], PageId pageCount) {
    std::lock_guard<std::mutex> lk(mu_);
    // The Pager already rebuilt our `allocated_` bitmap and `freeBuddy_`
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

} // namespace atomdb