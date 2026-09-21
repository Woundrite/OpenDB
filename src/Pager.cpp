#include "opendb/storage/Pager.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

#include "opendb/contracts/IPageAllocator.hpp"
#include "opendb/storage/BuddyPageAllocator.hpp"

namespace opendb {

namespace {
// v2 page-0 layout past the 28-byte fixed header (magic 4 + version 2 +
// reserved 2 + pageCount 4 + allocator scratch 16):
//   offset 32: u32 free-run count N (0..kMaxPersistedRuns)
//   offset 36: N x { u32 slabClass, u32 start } (8 bytes each)
constexpr std::size_t kFreeRunsOffset = 32;
constexpr std::size_t kMaxPersistedRuns =
    (Pager::PAGE_SIZE - kFreeRunsOffset - sizeof(std::uint32_t)) / 8;
} // namespace

Pager::Pager(const std::string& uri,
             std::unique_ptr<IPageAllocator> allocator)
    : allocator_(std::move(allocator)) {
    if (!allocator_) {
        allocator_ = std::make_unique<BuddyPageAllocator>();
    }
    openFile(uri);
    if (!loadHeader()) {
        // New file (or corrupted): initialize header.
        // loadHeader() left the stream in a bad state (failbit/eofbit) after the
        // short read on a 0-byte file. Clear before writing.
        file_.clear();
        header_.magic = MAGIC;
        header_.version = VERSION;
        header_.pageCount = 1; // page 0 exists
        header_.freeHead = INVALID_PAGE;
        std::memset(header_.allocatorHeader, 0, 16);
        flushHeader();
    }
}

Pager::~Pager() {
    close();
}

bool Pager::readPage(PageId id, std::vector<std::uint8_t>& out) {
    if (id == 0 || id >= header_.pageCount) return false;
    std::vector<std::uint8_t> raw;
    if (!readRaw(id, raw)) return false;

    // Verify CRC32 (first 4 bytes of the page).
    // CRC is computed only over the payload (offset DATA_PAGE_HEADER_SIZE .. end).
    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, raw.data(), 4);
    std::uint32_t computed = crc32(raw.data() + DATA_PAGE_HEADER_SIZE,
                                   raw.size() - DATA_PAGE_HEADER_SIZE);
    if (stored_crc != computed) return false;

    // Strip header, return payload.
    out.assign(raw.begin() + DATA_PAGE_HEADER_SIZE, raw.end());
    return true;
}

Pager::PageId Pager::writePage(std::optional<PageId> id,
                               const std::vector<std::uint8_t>& data) {
    if (data.size() != DATA_PAGE_PAYLOAD_SIZE) {
        throw std::runtime_error("Pager::writePage: data size must equal DATA_PAGE_PAYLOAD_SIZE");
    }
    PageId page_id;
    if (id.has_value()) {
        page_id = id.value();
        if (page_id == 0 || page_id >= header_.pageCount) {
            throw std::runtime_error("Pager::writePage: invalid page ID");
        }
    } else {
        page_id = allocatePage();
    }

    // Build full page with CRC32.
    std::vector<std::uint8_t> page(PAGE_SIZE);
    // Payload starts at offset DATA_PAGE_HEADER_SIZE.
    std::memcpy(page.data() + DATA_PAGE_HEADER_SIZE, data.data(), DATA_PAGE_PAYLOAD_SIZE);
    // Compute CRC32 over payload (matching the read-side range).
    std::uint32_t crc = crc32(page.data() + DATA_PAGE_HEADER_SIZE,
                              PAGE_SIZE - DATA_PAGE_HEADER_SIZE);
    std::memcpy(page.data(), &crc, 4);
    // Reserved bytes at offset 4..7 remain zero.

    writeRaw(page_id, page);
    return page_id;
}

void Pager::sync() {
    if (!file_.is_open()) return;
    file_.flush();
    fdatasyncFile();
}

void Pager::close() {
    if (file_.is_open()) {
        file_.flush();
        fdatasyncFile();
        // Write header one last time so allocator state is current.
        flushHeader();
        file_.close();
    }
}

bool Pager::readRaw(PageId id, std::vector<std::uint8_t>& buf) {
    if (id >= header_.pageCount) return false;
    buf.resize(PAGE_SIZE);
    file_.clear(); // Reset any stale eofbit/failbit before reading.
    file_.seekg(static_cast<std::streamoff>(id) * PAGE_SIZE, std::ios::beg);
    if (!file_) return false;
    file_.read(reinterpret_cast<char*>(buf.data()), PAGE_SIZE);
    return static_cast<std::size_t>(file_.gcount()) == PAGE_SIZE;
}

void Pager::writeRaw(PageId id, const std::vector<std::uint8_t>& data) {
    assert(data.size() == PAGE_SIZE);
    if (id >= header_.pageCount) {
        // Extending file: write zero pages up to id (shouldn't happen in normal flow).
        std::vector<std::uint8_t> zero(PAGE_SIZE, 0);
        file_.seekp(0, std::ios::end);
        while (header_.pageCount <= id) {
            file_.write(reinterpret_cast<const char*>(zero.data()), PAGE_SIZE);
            if (!file_) throw std::runtime_error("Pager::writeRaw: failed to extend file");
            ++header_.pageCount;
        }
        flushHeader();
    }
    file_.seekp(static_cast<std::streamoff>(id) * PAGE_SIZE, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(data.data()), PAGE_SIZE);
    if (!file_) throw std::runtime_error("Pager::writeRaw: write failed");
}

std::uint32_t Pager::crc32(const std::uint8_t* data, std::size_t len) {
    // CRC32 polynomial 0xEDB88320 (reversed), initial 0xFFFFFFFF, final XOR 0xFFFFFFFF.
    // Uses a small lookup table for speed.
    static const std::array<std::uint32_t, 256> TABLE = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = TABLE[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

Pager::PageId Pager::allocatePage() {
    return allocator_->allocatePages(1, [this](std::size_t n) {
        return extendFile(n);
    });
}

void Pager::freePage(PageId id) {
    if (id == 0 || id >= header_.pageCount) return;
    allocator_->freePages(id, 1);
}

std::vector<Pager::PageId> Pager::allocatePages(std::size_t n) {
    if (n == 0) return {};
    PageId start = allocator_->allocatePages(n, [this](std::size_t m) {
        return extendFile(m);
    });
    std::vector<PageId> result;
    result.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        result.push_back(start + i);
    }
    return result;
}

void Pager::freePages(PageId start, std::size_t n) {
    if (start == 0 || n == 0 || start + n > header_.pageCount) return;
    allocator_->freePages(start, n);
}

std::size_t Pager::freeListSize() {
    return allocator_->freePageCount();
}

std::size_t Pager::freeRunCount() {
    return allocator_->freeRunCount();
}

bool Pager::loadHeader() {
    std::vector<std::uint8_t> buf(PAGE_SIZE);
    file_.seekg(0, std::ios::beg);
    file_.read(reinterpret_cast<char*>(buf.data()), PAGE_SIZE);
    if (static_cast<std::size_t>(file_.gcount()) != PAGE_SIZE) return false;

    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::memcpy(&magic, buf.data(), 4);
    std::memcpy(&version, buf.data() + 4, 2);
    if (magic != MAGIC) return false;

    std::memcpy(&header_.pageCount, buf.data() + 8, 4);

    if (version == 1) {
        // v1: freeHead at offset 12, no allocator header.
        std::memcpy(&header_.freeHead, buf.data() + 12, 4);
        std::memset(header_.allocatorHeader, 0, 16);
        header_.version = 1;
        // Migrate to v2: rebuild allocator from freeHead chain, then bump version.
        migrateV1ToV2();
        return true;
    }

    if (version == 2) {
        // v2: allocatorHeader at offset 12 (16 bytes).
        std::memcpy(header_.allocatorHeader, buf.data() + 12, 16);
        header_.version = 2;
        header_.freeHead = INVALID_PAGE; // unused in v2
        // Load allocator state from the header bytes.
        allocator_->loadHeader(header_.allocatorHeader, header_.pageCount);
        // Restore the free-run snapshot when its count is structural;
        // per-run bounds are enforced by loadFreeRuns itself. A corrupt
        // count (or pre-persistence-era zeros) keeps the all-allocated
        // state — free pages leak safely, exactly as after a crash
        // before close().
        std::uint32_t runCount = 0;
        std::memcpy(&runCount, buf.data() + kFreeRunsOffset, 4);
        if (runCount <= kMaxPersistedRuns) {
            std::vector<IPageAllocator::FreeRun> runs;
            runs.reserve(runCount);
            for (std::uint32_t i = 0; i < runCount; ++i) {
                IPageAllocator::FreeRun r;
                std::memcpy(&r.slabClass,
                            buf.data() + kFreeRunsOffset + 4 + i * 8, 4);
                std::memcpy(&r.start,
                            buf.data() + kFreeRunsOffset + 4 + i * 8 + 4, 4);
                runs.push_back(r);
            }
            allocator_->loadFreeRuns(runs.data(), runs.size(),
                                     header_.pageCount);
        }
        return true;
    }

    return false; // unknown version
}

void Pager::migrateV1ToV2() {
    // Walk the v1 freeHead chain to rebuild allocator state.
    // 1. First, mark ALL data pages as allocated via onFileExtended.
    //    This ensures the allocator's internal bitmap is sized correctly.
    if (header_.pageCount > 1) {
        allocator_->onFileExtended(1, header_.pageCount - 1);
    }

    // 2. Walk the v1 freeHead chain to find which pages are actually free.
    std::vector<bool> isFree(header_.pageCount, false);
    PageId cur = header_.freeHead;
    std::unordered_set<PageId> seen;
    while (cur != INVALID_PAGE && cur != 0 && cur < header_.pageCount) {
        if (!seen.insert(cur).second) break; // cycle defense
        isFree[cur] = true;
        std::vector<std::uint8_t> pageBuf(PAGE_SIZE);
        if (!readRaw(cur, pageBuf)) break;
        std::memcpy(&cur, pageBuf.data(), 4);
    }

    // 3. Find contiguous free runs and call freePages for each.
    //    This will mark those pages as free and push them into the buddy structure.
    PageId runStart = INVALID_PAGE;
    std::size_t runLen = 0;
    for (PageId i = 1; i < header_.pageCount; ++i) {
        if (isFree[i]) {
            if (runLen == 0) {
                runStart = i;
                runLen = 1;
            } else {
                runLen++;
            }
        } else {
            if (runLen > 0) {
                allocator_->freePages(runStart, runLen);
                runLen = 0;
            }
        }
    }
    if (runLen > 0) {
        allocator_->freePages(runStart, runLen);
    }

    // 4. Bump version to v2 and flush header so on-disk format is v2
    header_.version = VERSION;
    flushHeader();
}

void Pager::flushHeader() {
    std::vector<std::uint8_t> page(PAGE_SIZE, 0);
    std::memcpy(page.data(), &header_.magic, 4);
    std::memcpy(page.data() + 4, &header_.version, 2);
    // reserved0 at [6..8) stays zero.
    std::memcpy(page.data() + 8, &header_.pageCount, 4);
    if (header_.version == 1) {
        std::memcpy(page.data() + 12, &header_.freeHead, 4);
    } else {
        // v2: allocator header at offset 12.
        allocator_->serializeHeader(header_.allocatorHeader);
        std::memcpy(page.data() + 12, header_.allocatorHeader, 16);
        // v2: free-run snapshot at offset 32. The whole page is rewritten
        // from zeros every flush, so a shrunken pool leaves no stale tail.
        // Runs past the cap are dropped: they leak as allocated after a
        // reopen (safe; identical to crashing before close()).
        std::vector<IPageAllocator::FreeRun> runs;
        allocator_->saveFreeRuns(runs);
        const std::uint32_t n = static_cast<std::uint32_t>(
            std::min<std::size_t>(runs.size(), kMaxPersistedRuns));
        std::memcpy(page.data() + kFreeRunsOffset, &n, 4);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::memcpy(page.data() + kFreeRunsOffset + 4 + i * 8,
                        &runs[i].slabClass, 4);
            std::memcpy(page.data() + kFreeRunsOffset + 4 + i * 8 + 4,
                        &runs[i].start, 4);
        }
    }
    writeRaw(0, page);
}

void Pager::fdatasyncFile() {
    file_.sync();
}

void Pager::openFile(const std::string& uri) {
    std::string path = uri;
    if (path.rfind("file://", 0) == 0) {
        path = path.substr(7);
    }
    path_ = path;

    // Open in binary mode, create if missing, read+write.
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        // Create new file.
        file_.open(path_, std::ios::out | std::ios::binary);
        file_.close();
        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open()) {
            throw std::runtime_error("Pager::openFile: cannot open/create " + path_);
        }
    }
    // std::ios::binary ensures no CRLF translation on Windows.
}

Pager::PageId Pager::extendFile(std::size_t n) {
    if (n == 0) return INVALID_PAGE;
    // Extend by n pages. The new pages start at current pageCount.
    PageId start = header_.pageCount;
    std::vector<std::uint8_t> zero(PAGE_SIZE, 0);
    file_.seekp(0, std::ios::end);
    for (std::size_t i = 0; i < n; ++i) {
        file_.write(reinterpret_cast<const char*>(zero.data()), PAGE_SIZE);
        if (!file_) throw std::runtime_error("Pager::extendFile: failed to extend file");
        ++header_.pageCount;
    }
    flushHeader();
    // Notify allocator of new allocated pages.
    allocator_->onFileExtended(start, n);
    return start;
}

} // namespace opendb