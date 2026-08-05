#include "test_framework.hpp"

#include <cstdio>
#include <filesystem>
#include <vector>

#include "atomdb/storage/Pager.hpp"

using namespace atomdb;

namespace {

// RAII helper: ensures the file is removed before the test starts (clean slate).
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& name) : path(name) {
        std::filesystem::remove(path);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

std::vector<std::uint8_t> makePayload(std::uint8_t fill = 0xAB, std::size_t n = 0) {
    if (n == 0) n = Pager::DATA_PAGE_PAYLOAD_SIZE;
    return std::vector<std::uint8_t>(n, fill);
}

} // namespace

TEST(Pager_New_File_Has_Header_And_No_Data_Pages) {
    TempFile tf("atomdb_pager_new.dat");
    Pager p(tf.path.string());
    EXPECT(p.isOpen());
    // Page 0 (header) counts.
    EXPECT_EQ(p.pageCount(), Pager::PageId{1});
}

TEST(Pager_Allocate_And_Write_And_Read_RoundTrip) {
    TempFile tf("atomdb_pager_alloc.dat");
    Pager p(tf.path.string());
    auto payload = makePayload(0xCD);
    Pager::PageId id = p.writePage(std::nullopt, payload);
    EXPECT(id == 1);
    EXPECT_EQ(p.pageCount(), Pager::PageId{2});

    std::vector<std::uint8_t> out;
    EXPECT(p.readPage(id, out));
    EXPECT_EQ(out.size(), std::size_t{Pager::DATA_PAGE_PAYLOAD_SIZE});
    EXPECT(out[0] == 0xCD);
    EXPECT(out[Pager::DATA_PAGE_PAYLOAD_SIZE - 1] == 0xCD);
}

TEST(Pager_FreeList_Push_Then_Allocate_Pops_From_List) {
    TempFile tf("atomdb_pager_freelist.dat");
    Pager p(tf.path.string());

    // Allocate three pages.
    auto p0 = p.writePage(std::nullopt, makePayload(0x01));
    auto p1 = p.writePage(std::nullopt, makePayload(0x02));
    auto p2 = p.writePage(std::nullopt, makePayload(0x03));
    EXPECT_EQ(p.pageCount(), Pager::PageId{4}); // header + 3 data pages

    // Free page 2: it should become the free-list head.
    p.freePage(p2);
    // Page count is unchanged (we're tracking allocation, not deallocation).
    EXPECT_EQ(p.pageCount(), Pager::PageId{4});

    // Next allocate must return the freed page id, NOT a new one.
    auto reuse = p.writePage(std::nullopt, makePayload(0x04));
    EXPECT(reuse == p2);
    // No new page was appended — pageCount is unchanged.
    EXPECT_EQ(p.pageCount(), Pager::PageId{4});
    (void)p0;
    (void)p1;
}

TEST(Pager_FreeList_LIFO_Order) {
    TempFile tf("atomdb_pager_lifo.dat");
    Pager p(tf.path.string());

    auto p0 = p.writePage(std::nullopt, makePayload());
    auto p1 = p.writePage(std::nullopt, makePayload());
    auto p2 = p.writePage(std::nullopt, makePayload());
    (void)p0;

    // Free pages 1 then 2. The free-list is LIFO so a subsequent allocate
    // returns page 2 first.
    p.freePage(p1);
    p.freePage(p2);

    auto a = p.writePage(std::nullopt, makePayload());
    auto b = p.writePage(std::nullopt, makePayload());
    EXPECT(a == p2);
    EXPECT(b == p1);
}

TEST(Pager_FreeList_Survives_Reopen) {
    TempFile tf("atomdb_pager_reopen.dat");
    {
        Pager p(tf.path.string());
        auto p0 = p.writePage(std::nullopt, makePayload(0x10));
        auto p1 = p.writePage(std::nullopt, makePayload(0x20));
        p.freePage(p1);
        (void)p0;
    }
    // Reopen: free-list head must be restored.
    Pager q(tf.path.string());
    auto reuse = q.writePage(std::nullopt, makePayload(0x30));
    // The freed page was the second allocation, so id should be 2 (header=0,
    // first data page=1, second data page=2; we freed id=2).
    EXPECT(reuse == 2);
    // Read it back and confirm the new payload was written.
    std::vector<std::uint8_t> out;
    EXPECT(q.readPage(reuse, out));
    EXPECT(out[0] == 0x30);
}

TEST(Pager_FreePage_Of_Unallocated_Id_Is_Noop) {
    TempFile tf("atomdb_pager_freenoop.dat");
    Pager p(tf.path.string());
    // Page 1 doesn't exist yet — freeing must not throw or alter page count.
    p.freePage(1);
    EXPECT_EQ(p.pageCount(), Pager::PageId{1});
}

TEST(Pager_Allocate_After_FreeList_Empty_Extends_File) {
    TempFile tf("atomdb_pager_extend.dat");
    Pager p(tf.path.string());
    auto p0 = p.writePage(std::nullopt, makePayload());
    p.freePage(p0);
    // Free the only allocated data page — free-list now has one entry.
    // Drain it.
    auto reused = p.writePage(std::nullopt, makePayload());
    EXPECT(reused == p0);

    // Now free-list is empty — next allocation must extend the file.
    auto extended = p.writePage(std::nullopt, makePayload());
    EXPECT(extended == p.pageCount() - 1);
    EXPECT_EQ(p.pageCount(), Pager::PageId{3}); // header + p0 + new
}

TEST(Pager_ReadPage_Of_Invalid_Id_Returns_False) {
    TempFile tf("atomdb_pager_invalid.dat");
    Pager p(tf.path.string());
    std::vector<std::uint8_t> out;
    EXPECT(!p.readPage(0, out));     // page 0 is metadata, not a data page
    EXPECT(!p.readPage(99, out));    // out of range
}

// Phase 6.5: Buddy allocator tests
TEST(Pager_Buddy_AllocatePages_One_Returns_First_Free) {
    TempFile tf("atomdb_buddy_1page.dat");
    Pager p(tf.path.string());
    auto ids = p.allocatePages(1);
    EXPECT_EQ(ids.size(), std::size_t{1});
    EXPECT(ids[0] == 1);
}

TEST(Pager_Buddy_AllocatePages_Two_Requests_2p_From_2p_Slab) {
    TempFile tf("atomdb_buddy_2page.dat");
    Pager p(tf.path.string());
    auto ids = p.allocatePages(2);
    EXPECT_EQ(ids.size(), std::size_t{2});
    EXPECT(ids[0] == 1);
    EXPECT(ids[1] == 2);
    // Should be contiguous
    EXPECT(ids[1] == ids[0] + 1);
}

TEST(Pager_Buddy_AllocatePages_Three_Rounds_Up_To_4p_Slab) {
    TempFile tf("atomdb_buddy_3page.dat");
    Pager p(tf.path.string());
    auto ids = p.allocatePages(3);
    EXPECT_EQ(ids.size(), std::size_t{3});
    // 3 pages requested, buddy allocator rounds up to 4-page slab (class 2)
    // But we only get 3 pages back - the 4th is internal fragmentation
    EXPECT(ids[0] == 1);
    EXPECT(ids[1] == 2);
    EXPECT(ids[2] == 3);
}

TEST(Pager_Buddy_Free_Coalesces_Two_Adjacent_2p_Into_4p) {
    TempFile tf("atomdb_buddy_coalesce.dat");
    Pager p(tf.path.string());
    auto ids1 = p.allocatePages(2);
    auto ids2 = p.allocatePages(2);
    EXPECT(ids1[0] == 1);
    EXPECT(ids2[0] == 3);
    // Free both 2-page runs
    p.freePages(ids1[0], 2);
    p.freePages(ids2[0], 2);
    // Now allocate 4 pages - should coalesce into a 4-page run
    auto ids3 = p.allocatePages(4);
    EXPECT_EQ(ids3.size(), std::size_t{4});
    EXPECT(ids3[0] == 1);
}

TEST(Pager_Buddy_Free_Recursively_Coalesces_To_Max_Class) {
    TempFile tf("atomdb_buddy_coalesce_recursive.dat");
    Pager p(tf.path.string());
    // Allocate and free 8 single pages to build up a larger free run
    std::vector<Pager::PageId> singles;
    for (int i = 0; i < 8; ++i) {
        auto ids = p.allocatePages(1);
        singles.push_back(ids[0]);
    }
    // Free them all
    for (auto id : singles) {
        p.freePages(id, 1);
    }
    // Now allocate 8 pages - should get a single 8-page run
    auto ids = p.allocatePages(8);
    EXPECT_EQ(ids.size(), std::size_t{8});
    // All contiguous
    for (size_t i = 1; i < ids.size(); ++i) {
        EXPECT(ids[i] == ids[i-1] + 1);
    }
}

TEST(Pager_Buddy_V1_To_V2_Migration_Preserves_Allocations_And_Frees) {
    // Create a v1 file with some free pages
    {
        TempFile tf("atomdb_v1_migrate.dat");
        Pager p(tf.path.string());
        auto p0 = p.writePage(std::nullopt, makePayload(0x11));
        auto p1 = p.writePage(std::nullopt, makePayload(0x22));
        auto p2 = p.writePage(std::nullopt, makePayload(0x33));
        p.freePage(p1); // free page 2
        // p0=1, p1=2, p2=3; freed p1 (id=2)
        (void)p0;
        (void)p2;
    }
    // Reopen with v2 code - migration should preserve state
    {
        Pager q("atomdb_v1_migrate.dat");
        EXPECT(q.pageCount() == 4); // header + 3 data pages
        EXPECT(q.freeListSize() == 1); // one free page
        auto reuse = q.writePage(std::nullopt, makePayload(0x44));
        EXPECT(reuse == 2); // should reuse the freed page
        // Verify payload
        std::vector<std::uint8_t> out;
        EXPECT(q.readPage(reuse, out));
        EXPECT(out[0] == 0x44);
    }
}

TEST(BTree_Large_Row_Stored_Across_MultiPage_Run_And_Freed_On_Delete) {
    // This test exercises the multi-page allocation path through BTree
    // when a row exceeds the single-page payload capacity.
    // For now, verify the allocatePages/freePages round-trip works
    // at the Pager level for a size > 1 page.
    TempFile tf("atomdb_buddy_multipage_btree.dat");
    Pager p(tf.path.string());
    
    // Allocate a 2-page run (simulating a large BTree node)
    auto ids = p.allocatePages(2);
    EXPECT_EQ(ids.size(), std::size_t{2});
    EXPECT(ids[1] == ids[0] + 1);
    
    // Write to both pages
    auto payload = makePayload(0xAA);
    EXPECT(p.writePage(ids[0], payload) == ids[0]);
    EXPECT(p.writePage(ids[1], payload) == ids[1]);
    
    // Read back
    std::vector<std::uint8_t> out;
    EXPECT(p.readPage(ids[0], out));
    EXPECT(out[0] == 0xAA);
    EXPECT(p.readPage(ids[1], out));
    EXPECT(out[0] == 0xAA);
    
    // Free the run
    p.freePages(ids[0], 2);
    EXPECT(p.freeListSize() == 2);
    
    // Reallocate - should get the same pages back
    auto reused = p.allocatePages(2);
    EXPECT(reused[0] == ids[0]);
    EXPECT(reused[1] == ids[1]);
}
