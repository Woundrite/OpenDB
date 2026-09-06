#include <cstdio>
#include <filesystem>
#include <vector>

#include "opendb/storage/Pager.hpp"

using namespace opendb;

int main() {
    std::filesystem::remove("test_buddy.dat");
    Pager p("test_buddy.dat");
    printf("Pager created, open=%d, pageCount=%u\n", p.isOpen(), p.pageCount());

    // Test allocatePage
    auto payload = std::vector<std::uint8_t>(Pager::DATA_PAGE_PAYLOAD_SIZE, 0xAB);
    Pager::PageId id1 = p.writePage(std::nullopt, payload);
    printf("Allocated page 1: %u, pageCount=%u\n", id1, p.pageCount());

    auto payload2 = std::vector<std::uint8_t>(Pager::DATA_PAGE_PAYLOAD_SIZE, 0xCD);
    Pager::PageId id2 = p.writePage(std::nullopt, payload2);
    printf("Allocated page 2: %u, pageCount=%u\n", id2, p.pageCount());

    // Test freePage
    p.freePage(id2);
    printf("Freed page 2, freeListSize=%zu\n", p.freeListSize());

    // Test allocatePages (new API)
    auto ids = p.allocatePages(2);
    printf("Allocated 2 pages: ");
    for (auto id : ids) printf("%u ", id);
    printf("\n");

    // Test freePages
    p.freePages(ids[0], 2);
    printf("Freed 2 pages, freeListSize=%zu\n", p.freeListSize());

    // Test reopen
    p.close();
    printf("Closed and reopened...\n");
    Pager q("test_buddy.dat");
    printf("Reopened, pageCount=%u, freeListSize=%zu\n", q.pageCount(), q.freeListSize());

    // Test freePageCount equivalence
    printf("freeRunCount=%zu\n", q.freeRunCount());

    q.close(); // release the OS file handle; Windows refuses remove() otherwise
    std::filesystem::remove("test_buddy.dat");
    return 0;
}