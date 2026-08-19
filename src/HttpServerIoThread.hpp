#ifndef ATOMDB_HTTP_SERVER_IO_THREAD_HPP
#define ATOMDB_HTTP_SERVER_IO_THREAD_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "atomdb/frontend/HttpServer.hpp"

namespace atomdb {

// Forward
class IoThread;

// Connection: per-socket state held inside IoThread.
struct Connection {
    HttpServer::SocketHandle fd = -1;
    std::string readBuffer;        // accumulator for incoming bytes
    std::string writeBuffer;       // accumulator for outgoing response
    std::size_t writeOffset = 0;   // bytes already written
    std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
    bool closed = false;
};

// IoThread: one per cpu-thread. Owns a subset of open Connection objects
// and runs a select() loop.
//
// Phase 6.6: the first io thread also watches the listen fd in its
// select() set. When the listen fd is readable, it calls accept() and
// round-robins the new connection to a peer io thread. This eliminates
// the separate accept thread that previously busy-spun on Windows
// because accept() on a non-blocking listen fd returns WSAEWOULDBLOCK.
class IoThread {
public:
    using HandlerFn = std::function<void(HttpServer::SocketHandle fd, const std::string& request)>;
    using CloseFn = std::function<void(HttpServer::SocketHandle fd)>;
    using AcceptFn = std::function<bool()>; // returns true if accepted, false if rejected (at capacity)

    // Construct an io thread. `isAcceptor` (default false) makes this
    // thread also watch the listen fd passed via setListenFd().
    // onAccept is called once per accepted connection — returns true if
    // accepted, false if rejected (server at capacity).
    // peerSelector returns the IoThread that should adopt the new fd.
    using PeerSelector = std::function<IoThread*(void)>;
    IoThread(HttpServer::Config cfg, HandlerFn onRequest, CloseFn onClose,
             AcceptFn onAccept = {},
             bool isAcceptor = false,
             PeerSelector peerSelector = {});
    ~IoThread();

    IoThread(const IoThread&) = delete;
    IoThread& operator=(const IoThread&) = delete;

    // Called from peer io thread (acceptor) to push a new connection.
    void adopt(HttpServer::SocketHandle fd);

    // Acceptor-only: hand this thread the listen fd so it can call
    // accept() whenever the fd becomes readable. Safe to call once,
    // before the loop starts.
    void setListenFd(HttpServer::SocketHandle fd);

    // Wake the select() loop immediately (so it picks up new connections
    // without waiting for the 100ms timeout).
    void wake();

    // Signal the io thread to exit; join happens in dtor.
    void stop();

    // Write response data to a specific fd owned by this io thread.
    void writeTo(HttpServer::SocketHandle fd, const std::string& data);

    void join();

private:
    void run();
    void handleRead(Connection& c);
    void handleWrite(Connection& c);
    void closeConnection(Connection& c);

    HttpServer::Config cfg_;
    HandlerFn onRequest_;
    CloseFn onClose_;
    AcceptFn onAccept_;
    PeerSelector peerSelector_;
    bool isAcceptor_ = false;
    HttpServer::SocketHandle listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex mu_; // guards conns_, wake_pipe_ writing
    std::unordered_map<HttpServer::SocketHandle, std::unique_ptr<Connection>> conns_;

#if defined(_WIN32)
    HttpServer::SocketHandle wake_read_fd_ = -1; // read end of wake pipe
#else
    int wake_read_fd_ = -1;  // read end of self-pipe
    int wake_write_fd_ = -1; // write end of self-pipe
#endif
};

} // namespace atomdb

#endif // ATOMDB_HTTP_SERVER_IO_THREAD_HPP
