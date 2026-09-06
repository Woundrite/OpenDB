#include "HttpServerIoThread.hpp"

#include "opendb/frontend/HttpServer.hpp"
#include "opendb/frontend/HttpSession.hpp"
#include "opendb/core/EngineDispatcher.hpp"
#include "opendb/frontend/SocketUtils.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace opendb {

using namespace sockets;

// ---------------------------------------------------------------------------
// IoThread
// ---------------------------------------------------------------------------

IoThread::IoThread(HttpServer::Config cfg, HandlerFn onRequest, CloseFn onClose,
                     AcceptFn onAccept,
                     bool isAcceptor, PeerSelector peerSelector)
    : cfg_(std::move(cfg)),
      onRequest_(std::move(onRequest)),
      onClose_(std::move(onClose)),
      onAccept_(std::move(onAccept)),
      peerSelector_(std::move(peerSelector)),
      isAcceptor_(isAcceptor) {
#if !defined(_WIN32)
    int pipefd[2];
    if (::pipe(pipefd) == 0) {
        wake_read_fd_  = pipefd[0];
        wake_write_fd_ = pipefd[1];
        setNonBlocking(wake_read_fd_);
        setNonBlocking(wake_write_fd_);
    }
#endif

    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

IoThread::~IoThread() {
    stop();
    join();
}

void IoThread::adopt(HttpServer::SocketHandle fd) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto c = std::make_unique<Connection>();
        c->fd = fd;
        conns_[fd] = std::move(c);
    }
    wake();
}

void IoThread::setListenFd(HttpServer::SocketHandle fd) {
    listen_fd_ = fd;
    wake();
}

void IoThread::wake() {
#if !defined(_WIN32)
    if (wake_write_fd_ >= 0) {
        const char byte = 'x';
        write(wake_write_fd_, &byte, 1);
    }
#endif
    // Windows: rely on 100ms select timeout; production uses IOCP.
}

void IoThread::stop() {
    running_.store(false);
    wake();
}

void IoThread::join() {
    if (thread_.joinable()) thread_.join();
}

void IoThread::writeTo(HttpServer::SocketHandle fd, const std::string& data) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = conns_.find(fd);
    if (it != conns_.end()) {
        it->second->writeBuffer = data;
        it->second->writeOffset = 0;
    }
}

void IoThread::run() {
    while (running_.load()) {
        fd_set readfds;
        fd_set writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        int maxFd = 0;
#if !defined(_WIN32)
        if (wake_read_fd_ >= 0) {
            FD_SET(wake_read_fd_, &readfds);
            maxFd = std::max(maxFd, wake_read_fd_);
        }
#endif

        // Phase 6.6: acceptor io thread also watches the listen fd.
        // Calling accept() only when select() says the listen fd is
        // readable avoids the Windows busy-spin where accept() on a
        // non-blocking listen fd returns WSAEWOULDBLOCK immediately.
        if (isAcceptor_ && listen_fd_ != static_cast<HttpServer::SocketHandle>(-1)) {
            FD_SET(listen_fd_, &readfds);
            maxFd = std::max(maxFd, static_cast<int>(listen_fd_));
        }

        std::vector<HttpServer::SocketHandle> writeFds;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& [fd, c] : conns_) {
                if (c->closed) continue;
                FD_SET(fd, &readfds);
                maxFd = std::max(maxFd, static_cast<int>(fd));
                if (!c->writeBuffer.empty() && c->writeOffset < c->writeBuffer.size()) {
                    FD_SET(fd, &writefds);
                    writeFds.push_back(fd);
                }
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms

        int rc = sockets::select(maxFd + 1, &readfds, &writefds, nullptr, &tv);
        if (rc < 0) {
#if defined(_WIN32)
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            break;
        }

        // Phase 6.6: drain the listen fd. On a busy-spin-free Windows
        // machine this fires only when a real connection is pending.
        if (isAcceptor_ && listen_fd_ != static_cast<HttpServer::SocketHandle>(-1) && FD_ISSET(listen_fd_, &readfds)) {
            while (running_.load()) {
                HttpServer::SocketHandle clientFd = -1;
                if (!sockets::accept(listen_fd_, clientFd)) break;
                bool accepted = false;
                if (onAccept_) accepted = onAccept_();
                if (!accepted) {
                    // I.6: At max connections — reject with 503.
                    static const std::string resp = "HTTP/1.1 503 Service Unavailable\r\n"
                                                    "Content-Type: application/json\r\n"
                                                    "Content-Length: 30\r\n"
                                                    "Connection: close\r\n"
                                                    "\r\n"
                                                    "{\"error\":\"server at capacity\"}";
                    sockets::write(clientFd, resp.c_str(), resp.size());
                    sockets::close(clientFd);
                    continue;
                }
                // Hand to a peer io thread (round-robin via the selector).
                IoThread* peer = peerSelector_ ? peerSelector_() : nullptr;
                if (peer) {
                    if (peer != this) peer->adopt(clientFd);
                    else adopt(clientFd);
                } else {
                    adopt(clientFd);
                }
            }
        }

        // Wake pipe: drain it.
#if !defined(_WIN32)
        if (wake_read_fd_ >= 0 && FD_ISSET(wake_read_fd_, &readfds)) {
            char buf[64];
            while (sockets::read(wake_read_fd_, buf, sizeof(buf)) > 0) {}
        }
#endif

        // Process read-ready sockets.
        std::vector<HttpServer::SocketHandle> readyReads;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& [fd, c] : conns_) {
                if (FD_ISSET(fd, &readfds)) readyReads.push_back(fd);
            }
        }
        for (auto fd : readyReads) {
            std::unique_ptr<Connection> c;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = conns_.find(fd);
                if (it == conns_.end()) continue;
                c = std::move(it->second);
            }
            handleRead(*c);
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!c->closed) {
                    conns_[fd] = std::move(c);
                } else {
                    closeConnection(*c);
                    conns_.erase(fd);
                }
            }
        }

        // Process write-ready sockets.
        for (auto fd : writeFds) {
            std::unique_ptr<Connection> c;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = conns_.find(fd);
                if (it == conns_.end()) continue;
                c = std::move(it->second);
            }
            if (FD_ISSET(fd, &writefds)) handleWrite(*c);
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!c->closed) {
                    conns_[fd] = std::move(c);
                } else {
                    closeConnection(*c);
                    conns_.erase(fd);
                }
            }
        }

        // Timeout-driven close: idle longer than readTimeout.
        auto now = std::chrono::steady_clock::now();
        std::vector<HttpServer::SocketHandle> idle;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& [fd, c] : conns_) {
                if (now - c->lastActivity > cfg_.readTimeout) {
                    idle.push_back(fd);
                }
            }
        }
        for (auto fd : idle) {
            std::unique_ptr<Connection> c;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = conns_.find(fd);
                if (it == conns_.end()) continue;
                c = std::move(it->second);
                conns_.erase(it);
            }
            closeConnection(*c);
        }
    }
}

void IoThread::handleRead(Connection& c) {
    char buf[4096];
    while (true) {
        int n = sockets::read(c.fd, buf, sizeof(buf));
        if (n > 0) {
            c.readBuffer.append(buf, static_cast<std::size_t>(n));
            c.lastActivity = std::chrono::steady_clock::now();
            continue;
        }
        if (n == 0) {
            // peer closed
            c.closed = true;
            return;
        }
#if defined(_WIN32)
        if (WSAGetLastError() == WSAEWOULDBLOCK) break;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
#endif
        c.closed = true;
        return;
    }

    // Look for end of headers (CRLFCRLF).
    auto headerEnd = c.readBuffer.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        // Headers incomplete; keep reading.
        if (c.readBuffer.size() > 64 * 1024) c.closed = true;
        return;
    }

    // Parse Content-Length if present.
    std::size_t contentLength = 0;
    auto headerSection = c.readBuffer.substr(0, headerEnd);
    auto clPos = headerSection.find("Content-Length:");
    if (clPos != std::string::npos) {
        auto start = headerSection.find_first_not_of(" \t", clPos + 14);
        auto end   = headerSection.find("\r\n", start);
        std::string cl = headerSection.substr(start, end - start);
        try { contentLength = std::stoull(cl); } catch (...) { contentLength = 0; }
    }
    std::size_t totalExpected = headerEnd + 4 + contentLength;
    if (c.readBuffer.size() < totalExpected) return;

    // We have a complete request.
    std::string request = c.readBuffer.substr(0, totalExpected);
    c.readBuffer.erase(0, totalExpected);

    if (onRequest_) onRequest_(c.fd, request);
}

void IoThread::handleWrite(Connection& c) {
    while (c.writeOffset < c.writeBuffer.size()) {
        int remaining = static_cast<int>(c.writeBuffer.size() - c.writeOffset);
        int n = sockets::write(c.fd,
                               c.writeBuffer.data() + c.writeOffset,
                               remaining);
        if (n > 0) {
            c.writeOffset += static_cast<std::size_t>(n);
            c.lastActivity = std::chrono::steady_clock::now();
            continue;
        }
        if (n < 0) {
#if defined(_WIN32)
            if (WSAGetLastError() == WSAEWOULDBLOCK) return;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
#endif
            c.closed = true;
            return;
        }
        c.closed = true;
        return;
    }
    // All bytes written; for HTTP/1.1 keep-alive we'd check Connection header.
    // v1 simply closes after one response.
    c.closed = true;
}

void IoThread::closeConnection(Connection& c) {
    sockets::close(c.fd);
    if (onClose_) onClose_(c.fd);
    c.closed = true;
}

} // namespace opendb