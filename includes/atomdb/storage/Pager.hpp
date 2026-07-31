#ifndef ATOMDB_PAGER_HPP
#define ATOMDB_PAGER_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
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

namespace atomdb {

// Pager: fixed-size 4 KiB page I/O with CRC32 tear detection and a free-list.
// Page 0 is the metadata page containing:
//   - 4 bytes: magic number (0x41544F4D = 'ATOM')
//   - 4 bytes: version (1)
//   - 8 bytes: total pages allocated
//   - 4 bytes: free-list head page ID (0 = empty)
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
    static constexpr std::uint16_t VERSION = 1;

    // Fixed header size on data pages (id >= 1).
    static constexpr std::size_t DATA_PAGE_HEADER_SIZE = 8; // CRC32 (4) + reserved (4)
    static constexpr std::size_t DATA_PAGE_PAYLOAD_SIZE = PAGE_SIZE - DATA_PAGE_HEADER_SIZE;

    // Open an existing file or create a new one.
    // `uri` is a file path (file:// prefix optional).
    Pager(const std::string& uri);

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

    // Allocate a new page: pop from free list if non-empty, else extend file.
    PageId allocatePage();

    // Free a page: push onto free list.
    void freePage(PageId id);

    // Phase 6.4: walk the free-list chain and return how many pages are
    // currently available for re-allocation. O(n) — tests only.
    std::size_t freeListSize();

private:
    // File header (page 0 layout).
    struct Header {
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint16_t reserved0 = 0;
        PageId pageCount = 0;
        PageId freeHead = INVALID_PAGE; // head of singly-linked free list
        // The rest of page 0 can hold a free-list bitmap or array. For v1 we
        // use a simple singly-linked list stored as a chain of page IDs in the
        // free pages themselves (each free page's first 4 bytes = next free).
    } header_;

    std::fstream file_;
    std::string path_;

    // Read raw page from disk (no CRC check). Returns false on failure.
    bool readRaw(PageId id, std::vector<std::uint8_t>& buf);

    // Write raw page to disk (with CRC inserted at offset 0).
    void writeRaw(PageId id, const std::vector<std::uint8_t>& data);

    // Compute CRC32 (polynomial 0xEDB88320, bit-reflected).
    static std::uint32_t crc32(const std::uint8_t* data, std::size_t len);

    // Platform-specific fdatasync/FlushFileBuffers on the underlying file handle.
    void fdatasyncFile();

    // Read header from page 0.
    bool loadHeader();

    // Write header to page 0.
    void flushHeader();

    // Open file with appropriate flags (binary, create if missing).
    void openFile(const std::string& uri);
};

} // namespace atomdb

#endif // ATOMDB_PAGER_HPP