// example/03_caching_wrapper.cpp
//
// Demonstrates composition of storage providers. The CachingStorageEngine
// sits between the dispatch engine and a slower backing engine, caching
// committed (visibleSeq == 0 in our world) gets and invalidating on put.
//
// Per spec §4.3, caching is an *engine* decorator, not a *provider*
// decorator — it sees keys/values, not schemas/tables. So we wrap the inner
// engine (LocalFileStorageProvider::engine()), not the provider itself.
//
// Compile (unix):
//   g++ -std=c++2b -Wall -Wextra -Wpedantic -Iincludes -pthread \
//        src/SqlParser.cpp example/03_caching_wrapper.cpp \
//        build/src/BTree.obj build/src/Pager.obj \
//        -o build/example_03
// Run:
//   ./build/example_03
//   atomdb> SELECT users WHERE id = 1     # peak cache: 1 hit
//   atomdb> SELECT users WHERE id = 1     # hit (cached)
//   atomdb> INSERT users ...              # invalidate
//   atomdb> SELECT users WHERE id = 1     # miss (cache cleared)

#include <iostream>
#include <memory>

#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/core/EngineLoop.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/frontend/ReplSource.hpp"
#include "atomdb/storage/CachingStorageEngine.hpp"
#include "atomdb/storage/LocalFileStorageProvider.hpp"

using namespace atomdb;

int main() {
    TransactionManager txnm;
    LockManager        lock_mgr;
    DeadlockDetector   deadlock(lock_mgr);

    // Inner: persistent storage.
    auto inner_provider = std::make_unique<LocalFileStorageProvider>();
    inner_provider->open("file://./example_03.atoms");

    // Schema (idempotent across runs).
    Schema users = {
        "users",
        {
            ColumnDef{"id",   ValueType::Int64, false, true,  0, {}, std::nullopt},
            ColumnDef{"name", ValueType::Text,  true,  false, 0, {}, std::nullopt},
        },
        std::nullopt,
    };
    {
        auto err = inner_provider->createTable(users);
        if (!err.isSentinel() && err.toString().find("already") == std::string::npos) {
            std::cerr << "createTable: " << err.toString() << "\n";
            return 1;
        }
    }

    // Outer: caching decorator. The Engine sees a cache, the cache sees a
    // LocalFile engine. LocalFileStorageProvider is the inner; we wrap it
    // BEFORE the engine is exposed.
    auto cache = std::make_unique<CachingStorageEngine>(inner_provider->engine());

    // Plug the cache into the EngineLoop exactly as before. The dispatch loop
    // doesn't know if it's talking to a cache or a raw engine.
    IStorageEngine& engine = *cache;
    ReplSource repl(std::cin, std::cout);
    EngineLoop loop(repl, engine, txnm, lock_mgr, deadlock);

    std::cout << "\natomdb caching example"
              << "\n  outer engine : CachingStorageEngine"
              << "\n  inner engine : LocalFileStorageProvider::engine()"
              << "\n  policy       : cache committed gets; invalidate on put"
              << "\n\n";

    loop.run();  // EXIT to quit

    inner_provider->close();
    std::cout << "bye\n";
    return 0;
}
