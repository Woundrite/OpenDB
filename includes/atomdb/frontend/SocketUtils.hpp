#ifndef ATOMDB_SOCKET_UTILS_HPP
#define ATOMDB_SOCKET_UTILS_HPP

#include "atomdb/frontend/HttpServer.hpp"

#include <fcntl.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace atomdb {
namespace sockets {

bool setNonBlocking(HttpServer::SocketHandle fd);
bool close(HttpServer::SocketHandle fd);
int read(HttpServer::SocketHandle fd, void* buf, int len);
int write(HttpServer::SocketHandle fd, const void* buf, int len);
bool bindAndListen(std::uint16_t port, HttpServer::SocketHandle& outFd, std::uint16_t& outPort);
bool accept(HttpServer::SocketHandle listenFd, HttpServer::SocketHandle& outFd);
int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout);

} // namespace sockets

} // namespace atomdb

#endif // ATOMDB_SOCKET_UTILS_HPP