// main.cpp — opendb launcher (composition root).
//
// Parses CLI flags, builds ONE shared core (TransactionManager, LockManager,
// DeadlockDetector) and ONE storage provider selected by URI scheme, then
// runs either the REPL (EngineLoop) or the HTTP server (EngineDispatcher +
// HttpServer) against them. Both paths share the same lock domain (G.3).
//
// Usage:
//   opendb                                    REPL on in-memory://
//   opendb --uri file://./db.dat              REPL on a file-backed provider
//   opendb --server --port 8080               HTTP server on in-memory://
//   opendb --uri sharded+file://./shard.db --shards 4
//
// The "registry" is makeProvider(): URI scheme -> provider instance. Adding
// a provider means adding one branch there; nothing else in the binary
// changes. (No dlopen-style dynamic loading — no consumer in v0.1.)

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "opendb/contracts/IStorageProvider.hpp"
#include "opendb/core/DeadlockDetector.hpp"
#include "opendb/core/EngineDispatcher.hpp"
#include "opendb/core/EngineLoop.hpp"
#include "opendb/core/LockManager.hpp"
#include "opendb/core/TransactionManager.hpp"
#include "opendb/frontend/HttpServer.hpp"
#include "opendb/frontend/ReplSource.hpp"
#include "opendb/storage/InMemoryStorageProvider.hpp"
#include "opendb/storage/LocalFileStorageProvider.hpp"
#include "opendb/storage/ShardedStorageProvider.hpp"

namespace {

void usage(std::ostream& out) {
    out << "opendb — embedded SQL engine\n"
        << "  --uri <uri>           storage URI (default in-memory://)\n"
        << "                        in-memory:// | file://<path> | sharded+file://<path>\n"
        << "  --shards <n>          shard count for sharded+file:// (required with it)\n"
        << "  --server              run the HTTP/JSON server instead of the REPL\n"
        << "  --port <n>            HTTP listen port (default 8080)\n"
        << "  --workers <n>         engine worker threads (default 4)\n"
        << "  --query-timeout <ms>  max cumulative execution time per session\n"
        << "                        (default 0 = off)\n"
        << "  --help                this text\n";
}

// URI scheme -> opened provider. Returns nullptr (message already printed)
// when the scheme is unknown or open() fails.
std::unique_ptr<opendb::IStorageProvider> makeProvider(const std::string& uri,
                                                       std::size_t shards) {
    using opendb::IStorageProvider;

    if (uri == "in-memory://") {
        auto p = std::make_unique<opendb::InMemoryStorageProvider>();
        if (!p->open(uri).isSentinel()) return nullptr;
        return p;
    }
    if (uri.rfind("file://", 0) == 0) {
        auto p = std::make_unique<opendb::LocalFileStorageProvider>();
        if (!p->open(uri).isSentinel()) {
            std::cerr << "opendb: cannot open '" << uri << "'\n";
            return nullptr;
        }
        return p;
    }
    if (uri.rfind("sharded+file://", 0) == 0) {
        if (shards == 0) {
            std::cerr << "opendb: sharded+file:// requires --shards <n>\n";
            return nullptr;
        }
        const std::string base = uri.substr(std::strlen("sharded+file://"));
        std::vector<std::unique_ptr<IStorageProvider>> children;
        for (std::size_t i = 0; i < shards; ++i) {
            auto shard = std::make_unique<opendb::LocalFileStorageProvider>();
            const std::string childUri =
                "file://" + base + "." + std::to_string(i);
            if (!shard->open(childUri).isSentinel()) {
                std::cerr << "opendb: cannot open shard '" << childUri << "'\n";
                return nullptr;
            }
            children.push_back(std::move(shard));
        }
        // Children are pre-opened on per-shard URIs; children_already_open
        // keeps the parent's open() from clobbering them with one URI.
        auto p = std::make_unique<opendb::ShardedStorageProvider>(
            std::move(children), /*children_already_open=*/true);
        if (!p->open(uri).isSentinel()) return nullptr;
        return p;
    }
    std::cerr << "opendb: unknown storage URI '" << uri << "'\n";
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    std::string uri = "in-memory://";
    std::size_t shards = 0;
    std::uint16_t port = 8080;
    std::size_t workers = 4;
    std::chrono::milliseconds queryTimeout(0);
    bool server = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "opendb: missing value for " << arg << "\n";
                usage(std::cerr);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "--server") {
            server = true;
            continue;
        }
        if (arg == "--uri") {
            uri = next();
            continue;
        }
        if (arg == "--shards") {
            shards = static_cast<std::size_t>(std::stoull(next()));
            continue;
        }
        if (arg == "--port") {
            port = static_cast<std::uint16_t>(std::stoull(next()));
            continue;
        }
        if (arg == "--workers") {
            workers = static_cast<std::size_t>(std::stoull(next()));
            continue;
        }
        if (arg == "--query-timeout") {
            queryTimeout = std::chrono::milliseconds(std::stoull(next()));
            continue;
        }
        std::cerr << "opendb: unknown flag '" << arg << "'\n";
        usage(std::cerr);
        return 2;
    }

    auto provider = makeProvider(uri, shards);
    if (!provider) return 1;

    // Shared core: one instance set for every front-end (G.3).
    opendb::TransactionManager txn_mgr;
    opendb::LockManager lock_mgr;
    opendb::DeadlockDetector deadlock_detector(lock_mgr);

    if (server) {
        opendb::EngineDispatcher dispatcher(workers, provider.get(),
                                            txn_mgr, lock_mgr,
                                            deadlock_detector,
                                            std::chrono::seconds(50),
                                            queryTimeout);
        opendb::HttpServer http(provider.get(), &dispatcher,
                                txn_mgr, lock_mgr, deadlock_detector);
        opendb::HttpServer::Config cfg;
        cfg.port = port;
        if (!http.listen(cfg)) {
            std::cerr << "opendb: cannot listen on port " << port << "\n";
            return 1;
        }
        std::cout << "opendb v0.1 http — listening on port " << http.boundPort()
                  << " (storage: " << uri << ").\n";
        http.join();  // blocks until stopped
        dispatcher.shutdown();
        provider->close();
        std::cout << "bye\n";
        return 0;
    }

    // REPL path: EngineLoop pulls commands from stdin against the shared core.
    opendb::ReplSource repl(std::cin, std::cout);
    opendb::EngineLoop engine(repl, *provider->engine(),
                              txn_mgr, lock_mgr, deadlock_detector);
    std::cout << "opendb v0.1 — storage: " << uri << " — type EXIT to quit\n";
    engine.run();
    provider->close();
    std::cout << "bye\n";
    return 0;
}
