#include "opendb/frontend/SocketUtils.hpp"

namespace opendb {
namespace sockets {

bool setNonBlocking(HttpServer::SocketHandle fd) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool close(HttpServer::SocketHandle fd) {
#if defined(_WIN32)
    return closesocket(static_cast<SOCKET>(fd)) == 0;
#else
    return ::close(fd) == 0;
#endif
}

int read(HttpServer::SocketHandle fd, void* buf, int len) {
#if defined(_WIN32)
    return recv(static_cast<SOCKET>(fd), static_cast<char*>(buf), len, 0);
#else
    return ::read(fd, buf, len);
#endif
}

int write(HttpServer::SocketHandle fd, const void* buf, int len) {
#if defined(_WIN32)
    return send(static_cast<SOCKET>(fd), static_cast<const char*>(buf), len, 0);
#else
    return ::write(fd, buf, len);
#endif
}

namespace {
bool g_wsaInitialized = false;
std::mutex g_wsaMu;

void ensureWsa() {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(g_wsaMu);
    if (!g_wsaInitialized) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        g_wsaInitialized = true;
    }
#endif
}
} // anonymous namespace

bool bindAndListen(std::uint16_t port, HttpServer::SocketHandle& outFd, std::uint16_t& outPort) {
    ensureWsa();
#if defined(_WIN32)
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (static_cast<SOCKET>(fd) == INVALID_SOCKET) return false;
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
#endif

    int yes = 1;
    setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(static_cast<int>(fd),
             reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(static_cast<HttpServer::SocketHandle>(fd));
        return false;
    }
    if (listen(static_cast<int>(fd), 128) < 0) {
        close(static_cast<HttpServer::SocketHandle>(fd));
        return false;
    }
    setNonBlocking(static_cast<HttpServer::SocketHandle>(fd));
    outFd = static_cast<HttpServer::SocketHandle>(fd);

    sockaddr_in boundAddr{};
    socklen_t len = sizeof(boundAddr);
    if (getsockname(static_cast<int>(fd),
                    reinterpret_cast<sockaddr*>(&boundAddr), &len) == 0) {
        outPort = ntohs(boundAddr.sin_port);
    } else {
        outPort = port;
    }
    return true;
}

bool accept(HttpServer::SocketHandle listenFd, HttpServer::SocketHandle& outFd) {
#if defined(_WIN32)
    SOCKET fd = ::accept(static_cast<SOCKET>(listenFd), nullptr, nullptr);
    if (fd == INVALID_SOCKET) return false;
#else
    int fd = ::accept(static_cast<int>(listenFd), nullptr, nullptr);
    if (fd < 0) return false;
#endif
    setNonBlocking(static_cast<HttpServer::SocketHandle>(fd));
    outFd = static_cast<HttpServer::SocketHandle>(fd);
    return true;
}

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout) {
    return ::select(nfds, readfds, writefds, exceptfds, timeout);
}

} // namespace sockets

} // namespace opendb