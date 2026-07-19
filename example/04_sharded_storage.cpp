// example/04_sharded_storage.cpp
//
// Hash-partitioned storage across N *persistent* child providers. Each shard
// is its own Local-file B-Tree, so data survives process restarts.
//
// Files written:
//   ./example_04_shard_0.atoms .. ./example_04_shard_3.atoms
//
// Hash routing uses FNV-1a over the key bytes (v0.1). The four shards route
// rows by `id mod 4` effectively; changing the partition column or shard
// count rebalances (and would require data movement — out of scope here).
//
// Compile (unix):
//   g++ -std=c++2b -Wall -Wextra -Wpedantic -Iincludes -pthread \
//        src/SqlParser.cpp example/04_sharded_storage.cpp \
//        -o build/example_04
// Run:
//   ./build/example_04
//   atomdb> INSERT users { id:1, name:alice }
//   atomdb> SELECT users
//   atomdb> EXIT
//   ./build/example_04     # rows survive
//
// Variations:
//   - Change makeShard() to swap any concrete provider (mix in-memory +
//     local-file across the four shards if you want).
//   - Change PartitionPolicy::Kind to None / Range / List — v0.1 only
//     implements Hash; the others are stubbed unless you extend
//     ShardedStorageEngine::routeByKey.

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/core/EngineLoop.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/frontend/ReplSource.hpp"
#include "atomdb/storage/InMemoryStorageProvider.hpp"
#include "atomdb/storage/LocalFileStorageProvider.hpp"
#include "atomdb/storage/ShardedStorageProvider.hpp"

using namespace atomdb;

// Factory: open one LocalFile shard rooted at the given URI. We use
// LocalFile here so each shard survives across process restarts. Swap to
// InMemory for a quick smoke-test.
static std::unique_ptr<IStorageProvider>
makeShard(const std::string& uri, int id) {
    auto p = std::make_unique<LocalFileStorageProvider>();
    if (auto err = p->open(uri); !err.isSentinel()) {
        std::cerr << "shard " << id << " open failed: "
                  << err.toString() << "\n";
        std::exit(1);
    }
    return p;
}

int main() {
    TransactionManager txnm;
    LockManager        lock_mgr;
    DeadlockDetector   deadlock(lock_mgr);

    // Define the shards upfront so we can also emit per-shard metadata.
    const int kShards = 4;
    std::vector<std::unique_ptr<IStorageProvider>> shards;
    for (int i = 0; i < kShards; ++i) {
        const std::string path =
            "./example_04_shard_" + std::to_string(i) + ".atoms";
        const bool exists = std::filesystem::exists(path);
        if (exists) {
            std::cout << "  shard " << i << ": " << path
                      << "  (reopen — data will load)\n";
        } else {
            std::cout << "  shard " << i << ": " << path
                      << "  (new file will be created)\n";
        }
        shards.push_back(makeShard("file://" + path, i));
    }

    auto sharded = std::make_unique<ShardedStorageProvider>(std::move(shards));

    // Schema: hash-partitioned on `id` with shardCount == kShards.
    Schema users = {
        "users",
        {
            ColumnDef{"id",   ValueType::Int64, false, true,  0, {}},
            ColumnDef{"name", ValueType::Text,  true,  false, 0, {}},
        },
        PartitionPolicy{
            PartitionPolicy::Kind::Hash,
            "id",                 // partition key column
            static_cast<std::uint32_t>(kShards),
            {},                   // no Range boundaries
            {},                   // no List groupings
        },
    };

    // The shards are already open from makeShard(). The sharded provider's
    // open() blindly re-opens each child with the supplied URI; since the
    // children are already open we pass an empty URI and let any
    // per-provider no-op semantics apply. (Children of LocalFile all share
    // the same "file://" scheme, so re-opening them by URI works trivially.)
    if (auto err = sharded->open(""); !err.isSentinel()) {
        std::cerr << "sharded open: " << err.toString() << "\n";
        return 1;
    }
    auto ct_err = sharded->createTable(users);
    if (!ct_err.isSentinel() &&
        ct_err.toString().find("already") == std::string::npos) {
        std::cerr << "createTable failed: " << ct_err.toString() << "\n";
        return 1;
    }

    IStorageEngine& engine = *sharded->engine();
    ReplSource repl(std::cin, std::cout);
    EngineLoop loop(repl, engine, txnm, lock_mgr, deadlock);

    std::cout << "\natomdb persistent-sharded example"
              << "\n  provider  : ShardedStorageProvider"
              << "\n  shards    : " << kShards
              << " x LocalFileStorageProvider (Pager-backed B-Tree)"
              << "\n  policy    : HASH(id) PARTITIONS " << kShards << "\n"
              << "  routing   : FNV-1a(key bytes) mod " << kShards << "\n"
              << "  durability: data on disk; survives process exit"
              << "\n\n";
    loop.run();  // type EXIT or Ctrl-D to quit

    // Explicit close — flushes any dirty pages via the inner engine's commit.
    sharded->close();
    std::cout << "bye\n";
    return 0;
}
