#ifndef ATOMDB_HTTP_SERVER_HPP
#define ATOMDB_HTTP_SERVER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "atomdb/contracts/IStorageProvider.hpp"
#include "atomdb/core/EngineDispatcher.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/DeadlockDetector.hpp"

namespace atomdb {

// Forward declarations for cross-TU types. Definitions live in HttpServer.cpp.
class IoThread;

// HttpServer: multi-threaded async HTTP/1.1 server. Architecture:
//   - N io threads (default = hardware_concurrency). Each io thread owns
//     a subset of open sockets and runs a select() loop with a 100ms
//     timeout. The first io thread also watches the listen fd in its
//     select() set and calls accept() when the listen fd is ready, then
//     round-robins the new connection to a peer io thread.
//   - Phase 6.6: previously we had a separate accept thread that
//     busy-spun on Windows because accept() on a non-blocking listen fd
//     returns WSAEWOULDBLOCK immediately. Now the listen fd is part of
//     the io thread's select() set, so accept() only fires when there is
//     actually a pending connection.
//   - On a ready socket the io thread reads bytes into a per-connection
//     read buffer, parses an HTTP/1.1 request, builds an HttpSession and
//     enqueues it on EngineDispatcher. The dispatcher worker executes
//     the SQL and writes the JSON response back via the io thread.
//
// Conflict resolution: clients send `_etag_<table>_<key>` in the request
// body for UPDATE/DELETE. The engine stamps every commit with an etag =
// monotonically-increasing commit seq. If the client's etag doesn't match
// the current version, the request is rejected with HTTP 409.
//
// Phase 5 Item 15 metrics are tracked through Stats.
class HttpServer {
public:
    using Clock = std::chrono::steady_clock;

    struct Config {
        std::uint16_t port = 8080;
        std::size_t ioThreadCount = 0;          // 0 => hardware_concurrency
        std::size_t maxConnections = 1024;
        std::chrono::milliseconds readTimeout{5000};
        std::chrono::milliseconds writeTimeout{5000};
        std::size_t maxBodySize = 10 * 1024 * 1024; // 10 MB default (I.7)
        // TLS configuration (optional). If tlsContext is non-null the
        // server wraps io reads/writes with SSL.
        void* tlsContext = nullptr;            // SSL_CTX* ; opaque in this header
    };

    struct Stats {
        std::atomic<std::uint64_t> connectionsAccepted{0};
        std::atomic<std::uint64_t> connectionsActive{0};
        std::atomic<std::uint64_t> requestsServed{0};
        std::atomic<std::uint64_t> requestsConflicted{0};
        std::atomic<std::uint64_t> requestsRejected{0};
        std::atomic<std::uint64_t> totalLatencyMicros{0};
        std::atomic<std::uint64_t> maxLatencyMicros{0};
    };

    // Constructor with shared core components
    HttpServer(IStorageProvider* storage,
               IEngineDispatcher* dispatcher,
               TransactionManager& txnm,
               LockManager& lkm,
               DeadlockDetector& dd);

    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool listen(const Config& cfg);
    void stop();
    void join();

    bool isListening() const noexcept { return running_.load(); }
    std::uint16_t boundPort() const noexcept { return bound_port_.load(); }
    const Stats& stats() const noexcept { return stats_; }

    // Phase 5 Item 15: lightweight per-process stats endpoint.
    std::string renderStatsSnapshot() const;

    // Socket type alias — exposed for IoThread friend usage.
#if defined(_WIN32)
    using SocketHandle = unsigned long long;
#else
    using SocketHandle = int;
#endif

private:
    IStorageProvider* storage_;
    IEngineDispatcher* dispatcher_;
    TransactionManager& txnm_;
    LockManager& lockMgr_;
    DeadlockDetector& deadlock_;
    Config cfg_{};
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> bound_port_{0};
    Stats stats_;
    std::atomic<std::size_t> activeConnections_{0};

    SocketHandle listen_fd_ = -1;
    std::vector<std::thread> io_threads_;
    std::vector<std::unique_ptr<class IoThread>> io_impls_;
    std::atomic<std::size_t> next_io_idx_{0};

    void onClientRequest(HttpServer::SocketHandle fd, const std::string& request);
    void sendResponse(SocketHandle fd, int statusCode, const std::string& reason, const std::string& body);
    void sendError(SocketHandle fd, int statusCode, const std::string& reason, const std::string& message);
    void handleQuery(SocketHandle fd, const std::string& bodyJson);
    void handleBegin(SocketHandle fd);
    void handleCommit(SocketHandle fd, const std::string& bodyJson);
    void handleRollback(SocketHandle fd, const std::string& bodyJson);
    void recordLatency(int statusCode);
    bool tryAcquireConnection();
    void releaseConnection();
};

// Render helpers (cross-TU).
std::string renderHttpResponse(int statusCode,
                               const std::string& reason,
                               const std::string& body,
                               const std::string& contentType = "application/json");

std::string renderHttpError(int statusCode,
                            const std::string& reason,
                            const std::string& message);

bool parseHttpHead(const std::string& headLine,
                   std::string& method,
                   std::string& path,
                   std::string& version);

} // namespace atomdb

#endif // ATOMDB_HTTP_SERVER_HPP
