#include "atomdb/storage/Pager.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <stdexcept>

namespace atomdb {

Pager::Pager(const std::string& uri) {
    openFile(uri);
    if (!loadHeader()) {
        // New file: initialize header.
        header_.magic = MAGIC;
        header_.version = VERSION;
        header_.pageCount = 1; // page 0 exists
        header_.freeHead = INVALID_PAGE;
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
    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, raw.data(), 4);
    std::uint32_t computed = crc32(raw.data() + 4, raw.size() - 4);
    if (stored_crc != computed) return false;

    // Strip header, return payload.
    out.assign(raw.begin() + DATA_PAGE_HEADER_SIZE, raw.end());
    return true;
}

Pager::PageId Pager::writePage(std::optional<PageId> id, const std::vector<std::uint8_t>& data) {
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
    // Compute CRC32 over payload.
    std::uint32_t crc = crc32(data.data(), DATA_PAGE_PAYLOAD_SIZE);
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
        file_.close();
    }
}

bool Pager::readRaw(PageId id, std::vector<std::uint8_t>& buf) {
    if (id >= header_.pageCount) return false;
    buf.resize(PAGE_SIZE);
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
    if (header_.freeHead != INVALID_PAGE) {
        PageId id = header_.freeHead;
        // Read the free page to get next pointer.
        std::vector<std::uint8_t> buf;
        readRaw(id, buf);
        PageId next = INVALID_PAGE;
        std::memcpy(&next, buf.data(), 4);
        header_.freeHead = next;
        flushHeader();
        return id;
    }
    // No free pages: extend file.
    PageId id = header_.pageCount++;
    flushHeader();
    return id;
}

void Pager::freePage(PageId id) {
    if (id == 0 || id >= header_.pageCount) return;
    // Write next pointer into the freed page.
    std::vector<std::uint8_t> page(PAGE_SIZE, 0);
    std::memcpy(page.data(), &header_.freeHead, 4);
    writeRaw(id, page);
    header_.freeHead = id;
    flushHeader();
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
    if (magic != MAGIC || version != VERSION) return false;

    std::memcpy(&header_.pageCount, buf.data() + 8, 4);
    std::memcpy(&header_.freeHead, buf.data() + 12, 4);
    return true;
}

void Pager::flushHeader() {
    std::vector<std::uint8_t> page(PAGE_SIZE, 0);
    std::memcpy(page.data(), &header_.magic, 4);
    std::memcpy(page.data() + 4, &header_.version, 2);
    std::memcpy(page.data() + 8, &header_.pageCount, 4);
    std::memcpy(page.data() + 12, &header_.freeHead, 4);
    writeRaw(0, page);
}

void Pager::fdatasyncFile() {
    // Portable: std::fstream::sync() calls pubsync() on the underlying buffer,
    // which on POSIX typically does fdatasync/fsync, on Windows does FlushFileBuffers.
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

} // namespace atomdb