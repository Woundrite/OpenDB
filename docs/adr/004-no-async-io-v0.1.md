# ADR-004: No async I/O in v0.1

## Status

Accepted (Phase 4 design decision; partially relaxed in Phase 5 with
the HttpServer multi-threaded architecture).

## Context

Asynchronous I/O (Linux `io_uring`, Windows IOCP, BSD kqueue, etc.)
gives the highest throughput per thread by avoiding the kernel/user
context-switch cost on every I/O op.

But:
- The MSYS2 g++ 14.2.0 toolchain on Windows does not ship `<net>` /
  `<networking>` C++23 networking headers.
- True async I/O is per-platform; cross-platform code needs a wrapper
  (ASIO / boost::beast / cpp-httplib).
- The user selected "C++23 native only" with no external dependencies.

## Decision

v0.1 uses **synchronous POSIX/Winsock sockets in a multi-threaded
select() loop** for the HTTP server:

- 1 accept thread runs `accept()` in a loop with non-blocking listen fd.
- N io threads each run `select(readfds, writefds, NULL, 100ms)`.
- Connections are dispatched round-robin from accept thread to io threads.
- Reads/writes block the io thread per connection, but connections are
  independent and parallelized across threads.

This is **not** truly async — it's "blocking I/O per thread". But it
satisfies the Phase 5 requirement for **multi-threaded async
concurrency** (concurrent clients, no head-of-line blocking) without
dragging in a third-party networking library.

## Consequences

- Throughput is bounded by `ioThreadCount * connections-per-thread`.
- For typical HTTP workloads (short requests, low concurrency), this is
  comfortably 1000+ req/s on commodity hardware.
- A future enhancement can swap the `select()` loop for `epoll` /
  `IOCP` / `kqueue` without changing the public API.
- The trade-off is deliberate: simple, cross-platform, no dependencies.
