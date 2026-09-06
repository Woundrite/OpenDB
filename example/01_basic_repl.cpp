// example/01_basic_repl.cpp
//
// Minimum viable OpenDB usage: InMemory storage, REPL front-end, REPL-driven
// EngineLoop. This is the "SQLite-style embedded mode" — single process, no
// separate server, type commands and read results on stdout.
//
// Compile (unix):
//   g++ -std=c++2b -Wall -Wextra -Wpedantic -Iincludes -pthread \
//        src/SqlParser.cpp example/01_basic_repl.cpp -o build/example_01
// Run:
//   ./build/example_01
//   opendb> INSERT users { id:1, name:nikhil, age:30 }
//   opendb> SELECT users
//   opendb> SELECT users WHERE id = 1
//   opendb> EXIT

#include <iostream>
#include <memory>

#include "opendb/core/DeadlockDetector.hpp"
#include "opendb/core/EngineLoop.hpp"
#include "opendb/core/LockManager.hpp"
#include "opendb/core/TransactionManager.hpp"
#include "opendb/frontend/ReplSource.hpp"
#include "opendb/storage/InMemoryStorageProvider.hpp"

using namespace opendb;

// A front-end receives Commands via ICommandSource::nextCommand() and presents
// results via present(ResultSet/DbError). The built-in ReplSource is line-based
// and ships a tiny grammar for INSERT/SELECT so we don't need to wire a SQL
// parser for this demo. (See example/02 for SQL syntax.)

// To exercise the storage layer directly (no EngineLoop), pass a Command to
// EngineLoop.dispatch() yourself. That path bypasses the ICommandSource
// entirely and is the closest thing to what a custom front-end would do.

int main() {
    // 1. Construct the four core singletons. The wiring order matters only for
    //    DeadlockDetector which references LockManager.
    TransactionManager txnm;
    LockManager        lock_mgr;
    DeadlockDetector   deadlock(lock_mgr);

    // 2. Pick a storage provider. InMemory is the zero-config default.
    auto storage = std::make_unique<InMemoryStorageProvider>();
    storage->open("in-memory://");
    // The IStorageEngine pointer returned by engine() is the Engine facet —
    // for InMemoryStorageProvider it IS the InMemoryStorageEngine.
    IStorageEngine& engine = *storage->engine();
    // downcast to InMemoryStorageEngine if you want access to the
    // tableCount/rowCount helpers; not needed for normal dispatch.

    // 3. Pre-register a schema so INSERTs know about columns. (ReplSource
    //    parses {key:val,...} literals; for richer types we use the SQL parser
    //    as in example/03.)
    storage->createTable({
        "users",
        {
            ColumnDef{"id",   ValueType::Int64, false, true,  0, {}, std::nullopt},
            ColumnDef{"name", ValueType::Text,  true,  false, 0, {}, std::nullopt},
            ColumnDef{"age",  ValueType::Int64, true,  false, 0, {}, std::nullopt},
        },
        std::nullopt,
    });

    // 4. Wire a front-end (the REPL reads stdin).
    ReplSource repl(std::cin, std::cout);

    // 5. EngineLoop glues everything: pulls Commands, dispatches, presents.
    EngineLoop loop(repl, engine, txnm, lock_mgr, deadlock);

    std::cout << "\nopendb sqlite-mode example"
              << "\n  storage    : in-memory"
              << "\n  capabilities: RandomAccess, OrderedScan, Concurrent"
              << "\n  schema     : users(id INT PK, name TEXT, age INT)"
              << "\n\n";
    loop.run();  // blocks until ReplSource returns nullopt (EXIT or EOF)
    std::cout << "bye\n";
    return 0;
}
