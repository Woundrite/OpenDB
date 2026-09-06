#ifndef OPENDB_PAGER_HPP
#define OPENDB_PAGER_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Cross-platform includes for fsync
#ifdef _WIN32
    #include <windows.h>
    #include <io.h>       // _get_osfhandle
    #include <fcntl.h>    // _O_BINARY
#else
    #include <fcntl.h>    // open, O_*
    #include <unistd.h>   // close, fsync, fdatasync
    #include <sys/stat.h> // S_IRUSR, S_IWUSR
#endif

#include "opendb/contracts/IPageAllocator.hpp"

namespace opendb {

// Pager: fixed-size 4 KiB page I/O with CRC32 tear detection and a pluggable
// free-list allocator. Page 0 is the metadata page containing:
//   - 4 bytes: magic number (0x41544F4D = 'ATOM')
//   - 4 bytes: version (1 = v1 freeHead; 2 = v2 with IPageAllocator bookkeeping)
//   - 8 bytes: total pages allocated
//   - 4 bytes: free-list head page ID (v1) OR allocator diagnostic fields (v2)
//   - remainder: free-list bitmap / array (rest of 4 KiB)
// Data pages (id >= 1) hold B-Tree nodes or slotted rows.
// Each data page has an 8-byte header: CRC32 + 4 bytes reserved.
// Remaining 4088 bytes are payload.
class Pager {
public:
    using PageId = std::uint32_t;
    static constexpr std::size_t PAGE_SIZE = 4096;
    static constexpr PageId INVALID_PAGE = 0xFFFFFFFF;
    static constexpr std::uint32_t MAGIC = 0x41544F4D; // "ATOM" little-endian
    static constexpr std::uint16_t VERSION = 2;        // v2: IPageAllocator bookkeeping

    // Fixed header size on data pages (id >= 1).
    static constexpr std::size_t DATA_PAGE_HEADER_SIZE = 8; // CRC32 (4) + reserved (4)
    static constexpr std::size_t DATA_PAGE_PAYLOAD_SIZE = PAGE_SIZE - DATA_PAGE_HEADER_SIZE;

    // Open an existing file or create a new one.
    // `uri` is a file path (file:// prefix optional).
    // `allocator` defaults to BuddyPageAllocator if null.
    explicit Pager(const std::string& uri,
                   std::unique_ptr<IPageAllocator> allocator = nullptr);

    ~Pager();  // defined in .cpp

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;
    Pager(Pager&&) = delete;
    Pager& operator=(Pager&&) = delete;

    // Read page `id` into `out`. Returns false if id out of range or CRC fails.
    bool readPage(PageId id, std::vector<std::uint8_t>& out);

    // Write `data` (must be exactly PAGE_SIZE) to page `id`.
    // If `id` == INVALID_PAGE, allocates a fresh page and writes.
    // Returns the page ID written (new or existing).
    PageId writePage(std::optional<PageId> id, const std::vector<std::uint8_t>& data);

    // Force all dirty pages to stable storage (fsync/fdatasync/FlushFileBuffers).
    void sync();

    // Close the file.
    void close();

    // Total pages currently allocated (including page 0).
    PageId pageCount() const noexcept { return header_.pageCount; }

    // Check if file is open.
    bool isOpen() const noexcept { return file_.is_open(); }

    // Allocate a single new page: forward to allocator's allocatePages(1).
    PageId allocatePage();

    // Free a single page: forward to allocator's freePages(id, 1).
    void freePage(PageId id);

    // Phase 6.5: allocate `n` contiguous pages (n >= 1). Returns the first
    // PageId of the run. Forward to allocator.
    std::vector<PageId> allocatePages(std::size_t n);

    // Phase 6.5: free a contiguous run of `n` pages starting at `start`.
    void freePages(PageId start, std::size_t n);

    // Phase 6.4: walk the free-list chain and return how many pages are
    // currently available for re-allocation. O(n) — tests only.
    // Delegates to allocator's freePageCount().
    std::size_t freeListSize();

    // Diagnostic: number of distinct free runs (test-only).
    std::size_t freeRunCount();

private:
    // File header (page 0 layout).
    struct Header {
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint16_t reserved0 = 0;
        PageId pageCount = 0;
        // v1: freeHead (offset 12) - head of singly-linked free list
        // v2: allocator diagnostic fields (16 bytes at offset 12):
        //   [0..7]: reserved (bitmap root)
        //   [8..9]: max observed slab class (u16)
        //   [10..11]: free run count (u16)
        //   [12..15]: reserved
        PageId freeHead = INVALID_PAGE;
        std::uint8_t allocatorHeader[16] = {0};
    } header_;

    std::fstream file_;
    std::string path_;
    std::unique_ptr<IPageAllocator> allocator_;

    // Read raw page from disk (no CRC check). Returns false on failure.
    bool readRaw(PageId id, std::vector<std::uint8_t>& buf);

    // Write raw page to disk (with CRC inserted at offset 0).
    void writeRaw(PageId id, const std::vector<std::uint8_t>& data);

    // Compute CRC32 (polynomial 0xEDB88320, bit-reflected).
    static std::uint32_t crc32(const std::uint8_t* data, std::size_t len);

    // Platform-specific fdatasync/FlushFileBuffers on the underlying file handle.
    void fdatasyncFile();

    // Read header from page 0. Handles v1 (freeHead chain) and v2 (allocator).
    bool loadHeader();

    // Write header to page 0 (includes allocatorHeader serialization).
    void flushHeader();

    // Open file with appropriate flags (binary, create if missing).
    void openFile(const std::string& uri);

    // Helper for v1->v2 migration: walk the freeHead chain, rebuild
    // allocator state, then bump header_.version = 2.
    void migrateV1ToV2();

    // Extend file by n pages and return first new PageId. Used by
    // IPageAllocator::extendFile callback.
    PageId extendFile(std::size_t n);
};

} // namespace opendb

#endif // OPENDB_PAGER_HPP