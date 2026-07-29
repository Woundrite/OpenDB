// example/02_persistent_file.cpp
//
// SQLite-style "open or create a file" persistence mode. Data lives in a
// `.adb` file on disk; reopen later and the previous rows are still there.
//
// Files written:
//
//   ./example_02.atoms         database file (Pager-managed 4 KiB pages)
//
// The InMemory and LocalFile providers implement the same IStorageProvider /
// IStorageEngine contract, so swapping providers is a one-line change — the
// EngineLoop, ICommandSource, TransactionManager, LockManager, and any
// IAccessPlugin are all oblivious.
//
// Compile (unix):
//   g++ -std=c++2b -Wall -Wextra -Wpedantic -Iincludes -pthread \
//        src/SqlParser.cpp example/02_persistent_file.cpp -o build/example_02
// Run:
//   ./build/example_02
//   atomdb> INSERT users { id:1, name:persisted }
//   atomdb> SELECT users
//   atomdb> EXIT
//   ./build/example_02     # rows are still there
//
// In-memory equivalent (for comparison): swap the storage provider line for
//     auto storage = std::make_unique<InMemoryStorageProvider>();
//     storage->open("in-memory://");

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>

#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/core/EngineLoop.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/frontend/ReplSource.hpp"
#include "atomdb/storage/LocalFileStorageProvider.hpp"

using namespace atomdb;

int main() {
    namespace fs = std::filesystem;
    const char* path = "./example_02.atoms";
    const bool existed = fs::exists(path);

    // Core singletons: identical to example 1.
    TransactionManager txnm;
    LockManager        lock_mgr;
    DeadlockDetector   deadlock(lock_mgr);

    // Provider swap: LocalFile for crash-durable persistence. The Pager
    // underneath the BTree writes 4 KiB pages with CRC32 checksums into the
    // file.
    auto storage = std::make_unique<LocalFileStorageProvider>();
    auto open_err = storage->open(std::string("file://") + path);
    if (!open_err.isSentinel()) {
        std::cerr << "open() failed: " << open_err.toString() << "\n";
        return 1;
    }

    IStorageEngine& engine = *storage->engine();

    // Pre-register the schema. On a fresh file this adds the table; on reopen
    // createTable fails (duplicate) which is fine — we ignore the error.
    Schema users = {
        "users",
        {
            ColumnDef{"id",   ValueType::Int64, false, true,  0, {}, std::nullopt},
            ColumnDef{"name", ValueType::Text,  true,  false, 0, {}, std::nullopt},
        },
        std::nullopt,
    };
    if (auto err = storage->createTable(users); !err.isSentinel() &&
        err.toString().find("already") == std::string::npos) {
        // any error other than "already exists" is fatal
        std::cerr << "createTable failed: " << err.toString() << "\n";
        return 1;
    }

    ReplSource repl(std::cin, std::cout);
    EngineLoop loop(repl, engine, txnm, lock_mgr, deadlock);

    std::cout << "\natomdb persistent-mode example"
              << "\n  storage : LocalFileStorageProvider (Pager-backed B-Tree)"
              << "\n  file    : " << path << (existed ? " (reopened)" : " (new)")
              << "\n  schema  : users(id INT PK, name TEXT)"
              << "\n\n";
    loop.run();  // type EXIT to quit

    // Always close — flushes dirty pages to disk via storage->engine()->commit
    // when last visible write finishes. (EngineLoop already commits each auto-
    // commit DML on success, so on a clean EXIT the file is consistent.)
    if (auto err = storage->close(); !err.isSentinel()) {
        std::cerr << "close() warning: " << err.toString() << "\n";
    }
    std::cout << "bye (file flushed)\n";
    return 0;
}
