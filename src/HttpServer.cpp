#include "atomdb/frontend/HttpServer.hpp"
#include "atomdb/frontend/HttpSession.hpp"
#include "atomdb/frontend/JsonEncoder.hpp"
#include "atomdb/frontend/HttpApi.hpp"
#include "atomdb/core/EngineDispatcher.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/DeadlockDetector.hpp"
#include "HttpServerIoThread.hpp"
#include "atomdb/frontend/SocketUtils.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace atomdb {

// Global helpers for HTTP rendering (referenced by HttpServer members).
std::string renderHttpResponse(int statusCode,
                               const std::string& reason,
                               const std::string& body,
                               const std::string& contentType) {
    std::ostringstream os;
    os << "HTTP/1.1 " << statusCode << ' ' << reason << "\r\n"
       << "Content-Type: " << contentType << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n"
       << "\r\n"
       << body;
    return os.str();
}

std::string renderHttpError(int statusCode,
                            const std::string& reason,
                            const std::string& message) {
    std::ostringstream os;
    os << "HTTP/1.1 " << statusCode << ' ' << reason << "\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << (20 + message.size()) << "\r\n"
       << "Connection: close\r\n"
       << "\r\n"
       << "{\"error\":\"" << message << "\"}";
    return os.str();
}

namespace {

bool parseHttpHead(const std::string& headLine,
                   std::string& method,
                   std::string& path,
                   std::string& version) {
    auto p1 = headLine.find(' ');
    if (p1 == std::string::npos) return false;
    auto p2 = headLine.find(' ', p1 + 1);
    if (p2 == std::string::npos) return false;
    method  = headLine.substr(0, p1);
    path    = headLine.substr(p1 + 1, p2 - p1 - 1);
    version = headLine.substr(p2 + 1);
    return true;
}

bool splitMethodPath(const std::string& requestLine,
                     std::string& method,
                     std::string& path) {
    std::string version;
    if (!parseHttpHead(requestLine, method, path, version)) return false;
    auto q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

HttpServer::HttpServer(IStorageProvider* storage, IEngineDispatcher* dispatcher)
    : storage_(storage), dispatcher_(dispatcher) {}

HttpServer::~HttpServer() {
    stop();
    join();
}

bool HttpServer::listen(const Config& cfg) {
    if (running_.load()) return true;
    cfg_ = cfg;
    if (cfg_.ioThreadCount == 0) {
        cfg_.ioThreadCount = std::thread::hardware_concurrency();
    }
    if (cfg_.ioThreadCount == 0) cfg_.ioThreadCount = 1;

    // Bind listening socket.
    std::uint16_t boundPort = 0;
    if (!sockets::bindAndListen(cfg_.port, listen_fd_, boundPort)) {
        return false;
    }
    bound_port_.store(boundPort);

    // Create io threads.
    io_impls_.reserve(cfg_.ioThreadCount);
    for (std::size_t i = 0; i < cfg_.ioThreadCount; ++i) {
        io_impls_.push_back(std::make_unique<IoThread>(
            cfg_,
            [this](SocketHandle fd, const std::string& request) {
                onClientRequest(fd, request);
            },
            [this](SocketHandle /*fd*/) {
                // Client closed; no action needed.
            }
        ));
    }

    // Start accept loop.
    running_.store(true);
    accept_thread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::acceptLoop() {
    while (running_.load()) {
        SocketHandle clientFd = -1;
        if (!sockets::accept(listen_fd_, clientFd)) {
            if (!running_.load()) break;
            continue;
        }
        stats_.connectionsAccepted.fetch_add(1);
        stats_.connectionsActive.fetch_add(1);

        // Round-robin to io thread.
        std::size_t idx = next_io_idx_.fetch_add(1) % io_impls_.size();
        io_impls_[idx]->adopt(clientFd);
    }
}

void HttpServer::stop() {
    running_.store(false);
    if (listen_fd_ != static_cast<SocketHandle>(-1)) {
        sockets::close(listen_fd_);
        listen_fd_ = static_cast<SocketHandle>(-1);
    }
    for (auto& impl : io_impls_) {
        impl->stop();
    }
}

void HttpServer::join() {
    if (accept_thread_.joinable()) accept_thread_.join();
    for (auto& impl : io_impls_) {
        impl->join();
    }
}

void HttpServer::onClientRequest(SocketHandle fd, const std::string& request) {
    auto start = std::chrono::steady_clock::now();

    // Parse HTTP request line.
    auto crlf = request.find("\r\n");
    if (crlf == std::string::npos) {
        sendError(fd, 400, "Bad Request", "invalid request line");
        return;
    }
    std::string method, path;
    if (!splitMethodPath(request.substr(0, crlf), method, path)) {
        sendError(fd, 400, "Bad Request", "invalid request line");
        return;
    }

    // Route: POST /query, POST /begin, POST /commit, POST /rollback,
    // GET /health, GET /metrics, GET /status.
    std::string bodyJson;
    auto headerEnd = request.find("\r\n\r\n");
    if (headerEnd != std::string::npos && request.size() > headerEnd + 4) {
        bodyJson = request.substr(headerEnd + 4);
    }

    auto recordLatency = [&](int sc) {
        auto end = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        stats_.totalLatencyMicros.fetch_add(static_cast<std::uint64_t>(us));
        // atomic fetch_max requires C++17; use compare_exchange loop
        auto maxVal = stats_.maxLatencyMicros.load();
        while (maxVal < static_cast<std::uint64_t>(us) &&
               !stats_.maxLatencyMicros.compare_exchange_weak(maxVal, static_cast<std::uint64_t>(us))) {
            // retry
        }
        if (sc >= 200 && sc < 300) stats_.requestsServed.fetch_add(1);
        else if (sc == 409) stats_.requestsConflicted.fetch_add(1);
        else stats_.requestsRejected.fetch_add(1);
    };

    if (method == "POST" && path == "/query") {
        handleQuery(fd, bodyJson);
    } else if (method == "POST" && path == "/begin") {
        handleBegin(fd);
    } else if (method == "POST" && path == "/commit") {
        handleCommit(fd, bodyJson);
    } else if (method == "POST" && path == "/rollback") {
        handleRollback(fd, bodyJson);
    } else if (method == "GET" && (path == "/health" || path == "/healthz")) {
        sendResponse(fd, 200, "OK", "{\"status\":\"ok\"}");
    } else if (method == "GET" && path == "/metrics") {
        sendResponse(fd, 200, "OK", renderStatsSnapshot());
    } else if (method == "GET" && path == "/status") {
        sendResponse(fd, 200, "OK",
            "{\"connections\":" + std::to_string(stats_.connectionsActive.load()) +
            ",\"requests\":" + std::to_string(stats_.requestsServed.load()) + "}");
    } else {
        recordLatency(404);
        sendError(fd, 404, "Not Found", "endpoint not found: " + method + " " + path);
        return;
    }

    recordLatency(200);
}

void HttpServer::sendResponse(SocketHandle fd, int statusCode,
                              const std::string& reason,
                              const std::string& body) {
    auto res = renderHttpResponse(statusCode, reason, body, "application/json");
    for (auto& impl : io_impls_) {
        impl->writeTo(fd, res);
    }
}

void HttpServer::sendError(SocketHandle fd, int statusCode,
                           const std::string& reason, const std::string& message) {
    auto res = renderHttpError(statusCode, reason, message);
    for (auto& impl : io_impls_) {
        impl->writeTo(fd, res);
    }
}

// ---------------------------------------------------------------------------
// Endpoint handlers
// ---------------------------------------------------------------------------

void HttpServer::handleQuery(SocketHandle fd, const std::string& bodyJson) {
    HttpApiAccessPlugin plugin;
    TransactionManager txnm;
    LockManager lkm;
    DeadlockDetector dd(lkm);
    DbError err = plugin.open("", storage_, dispatcher_);
    if (!err.isSentinel()) {
        std::string json = JsonEncoder::encode(err);
        for (auto& impl : io_impls_) {
            impl->writeTo(fd, renderHttpResponse(500, "Internal Error", json, "application/json"));
        }
        return;
    }
    std::string response = plugin.handleRequest(bodyJson);
    for (auto& impl : io_impls_) {
        impl->writeTo(fd, renderHttpResponse(200, "OK", response, "application/json"));
    }
    plugin.close();
}

void HttpServer::handleBegin(SocketHandle fd) {
    HttpApiAccessPlugin plugin;
    TransactionManager txnm;
    LockManager lkm;
    DeadlockDetector dd(lkm);
    DbError err = plugin.open("", storage_, dispatcher_);
    if (!err.isSentinel()) {
        std::string json = JsonEncoder::encode(err);
        for (auto& impl : io_impls_) {
            impl->writeTo(fd, renderHttpResponse(500, "Internal Error", json, "application/json"));
        }
        return;
    }
    std::string response = plugin.handleRequest("{\"type\":\"begin\"}");
    for (auto& impl : io_impls_) {
        impl->writeTo(fd, renderHttpResponse(200, "OK", response, "application/json"));
    }
    plugin.close();
}

void HttpServer::handleCommit(SocketHandle fd, const std::string& bodyJson) {
    HttpApiAccessPlugin plugin;
    TransactionManager txnm;
    LockManager lkm;
    DeadlockDetector dd(lkm);
    DbError err = plugin.open("", storage_, dispatcher_);
    if (!err.isSentinel()) {
        std::string json = JsonEncoder::encode(err);
        for (auto& impl : io_impls_) {
            impl->writeTo(fd, renderHttpResponse(500, "Internal Error", json, "application/json"));
        }
        return;
    }
    std::string response = plugin.handleRequest(bodyJson);
    for (auto& impl : io_impls_) {
        impl->writeTo(fd, renderHttpResponse(200, "OK", response, "application/json"));
    }
    plugin.close();
}

void HttpServer::handleRollback(SocketHandle fd, const std::string& bodyJson) {
    HttpApiAccessPlugin plugin;
    TransactionManager txnm;
    LockManager lkm;
    DeadlockDetector dd(lkm);
    DbError err = plugin.open("", storage_, dispatcher_);
    if (!err.isSentinel()) {
        std::string json = JsonEncoder::encode(err);
        for (auto& impl : io_impls_) {
            impl->writeTo(fd, renderHttpResponse(500, "Internal Error", json, "application/json"));
        }
        return;
    }
    std::string response = plugin.handleRequest(bodyJson);
    for (auto& impl : io_impls_) {
        impl->writeTo(fd, renderHttpResponse(200, "OK", response, "application/json"));
    }
    plugin.close();
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

std::string HttpServer::renderStatsSnapshot() const {
    std::ostringstream os;
    os << "{\"connections_accepted\":" << stats_.connectionsAccepted.load()
       << ",\"connections_active\":" << stats_.connectionsActive.load()
       << ",\"requests_served\":" << stats_.requestsServed.load()
       << ",\"requests_conflicted\":" << stats_.requestsConflicted.load()
       << ",\"requests_rejected\":" << stats_.requestsRejected.load()
       << ",\"total_latency_us\":" << stats_.totalLatencyMicros.load()
       << ",\"max_latency_us\":" << stats_.maxLatencyMicros.load()
       << "}";
    return os.str();
}

} // namespace atomdb