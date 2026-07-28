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
// and runs a select() loop. The accept thread hands new fds via a
// wake-pipe (self-pipe trick on POSIX, eventfd-equivalent on Winsock).
class IoThread {
public:
    using HandlerFn = std::function<void(HttpServer::SocketHandle fd, const std::string& request)>;
    using CloseFn = std::function<void(HttpServer::SocketHandle fd)>;

    IoThread(HttpServer::Config cfg, HandlerFn onRequest, CloseFn onClose);
    ~IoThread();

    IoThread(const IoThread&) = delete;
    IoThread& operator=(const IoThread&) = delete;

    // Called from accept thread to push a new connection to this io thread.
    void adopt(HttpServer::SocketHandle fd);

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
