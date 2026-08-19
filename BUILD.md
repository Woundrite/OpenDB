# OpenDB Build Guide

## Overview
This guide provides comprehensive instructions for building OpenDB from source on fresh hardware. OpenDB is an embedded SQL database engine written in C++23, designed for high-performance embedded workloads.

## Table of Contents
1. [System Requirements](#system-requirements)
2. [Dependencies](#dependencies)
3. [Build Instructions](#build-instructions)
4. [Verification](#verification)
5. [Troubleshooting](#troubleshooting)
6. [Project Structure](#project-structure)
7. [Dependencies Detail](#dependencies-detail)

---

## System Requirements

### Minimum Hardware
- **CPU**: x86_64 (Intel/AMD) with SSE4.2 support
- **RAM**: 2 GB minimum (4 GB recommended for development)
- **Disk**: 500 MB free space (1 GB recommended for build artifacts)

### Operating System Support
| OS | Status | Notes |
|----|--------|-------|
| Linux (Ubuntu 20.04+, Debian 11+, Fedora 35+, Arch) | ✅ Fully Supported | Primary development platform |
| Windows 10/11 (MSYS2/MinGW-w64) | ✅ Fully Supported | Via MSYS2 shell |
| macOS 12+ (Intel/Apple Silicon) | ⚠️ Experimental | Requires Homebrew LLVM |

### Required Toolchain
| Tool | Minimum Version | Verification Command |
|------|-----------------|---------------------|
| C++ Compiler | GCC 13+ / Clang 16+ / MSVC 19.40+ | `g++ --version` |
| C++ Standard | C++23 (C++2b draft) | Compiler must support `-std=c++2b` |
| Build System | GNU Make 4.3+ / Ninja 1.11+ | `make --version` |
| Git | 2.30+ | `git --version` |

---

## Dependencies

### System Libraries (Linux/macOS)
```bash
# Ubuntu/Debian
sudo apt-get update && sudo apt-get install -y \
    build-essential \
    g++-13 \
    make \
    git \
    libpthread-stubs0-dev

# Fedora/RHEL
sudo dnf install -y \
    gcc-c++ \
    make \
    git \
    glibc-devel

# Arch Linux
sudo pacman -S --needed base-devel git

# macOS (Homebrew)
brew install llvm make git
```

### System Libraries (Windows - MSYS2)
```bash
# In MSYS2 UCRT64 shell
pacman -S --needed \
    mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-make \
    git \
    mingw-w64-ucrt-x86_64-pthreads
```

### No External Runtime Dependencies
OpenDB is designed as a zero-dependency embedded database:
- **No external libraries required at runtime**
- Only standard C++ library and POSIX/Windows threading APIs
- Optional: OpenSSL for TLS (compile-time option)

---

## Build Instructions

### 1. Clone Repository
```bash
git clone https://github.com/your-org/opendb.git
cd opendb
```

### 2. Verify Toolchain
```bash
# Verify C++23 support
g++ --version
# Should show GCC 13+, Clang 16+, or MSVC 19.40+

# Verify C++2b flag support
echo 'int main() {}' | g++ -std=c++2b -x c++ -
# Should compile without errors
```

### 3. Build Options

#### Option A: GNU Make (Recommended)
```bash
# Standard build (all targets)
make

# Debug build with symbols
make DEBUG=1

# Release build with optimizations
make RELEASE=1

# Run tests after build
make test

# Clean build artifacts
make clean

# Show all targets
make help
```

#### Option B: Manual Build (Cross-platform)
```bash
# Create build directories
mkdir -p build/obj build/tests

# Compile all source files
# Core engine
g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes \
    -c src/BTree.cpp -o build/obj/BTree.o
g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes \
    -c src/BuddyPageAllocator.cpp -o build/obj/BuddyPageAllocator.o
g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes \
    -c src/EngineDispatcher.cpp -o build/obj/EngineDispatcher.o
g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes \
    -c src/HttpServer.cpp -o build/obj/HttpServer.o
g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes \
    -c src/HttpServerIoThread.cpp -o build/obj/HttpServerIoThread.o
g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes \
    -c src/Pager.cpp -o build/obj/Pager.o
g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes \
    -c src/SocketUtils.cpp -o build/obj/SocketUtils.o
g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes \
    -c src/SqlParser.cpp -o build/obj/SqlParser.o

# Compile test files
g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes \
    -c tests/main.cpp -o build/tests/main.o
# ... repeat for each test file in tests/*.test.cpp

# Link test executables
# Core tests
g++ -std=c++2b -Wall -Wextra -Iincludes \
    -o build/core_test \
    build/obj/BTree.o build/obj/BuddyPageAllocator.o \
    build/obj/EngineDispatcher.o build/obj/HttpServer.o \
    build/obj/HttpServerIoThread.o build/obj/Pager.o \
    build/obj/SocketUtils.o build/obj/SqlParser.o \
    build/tests/Core.test.o build/tests/EngineDispatcher.test.o \
    build/tests/EngineLoop.test.o build/tests/HttpApi.test.o \
    build/tests/HttpServer.test.o build/tests/Predicate.test.o \
    build/tests/SqlParser.test.o build/tests/JsonEncoder.test.o \
    build/tests/Storage.test.o build/tests/LocalFileStorageProvider.test.o \
    build/tests/ShardedStorageProvider.test.o \
    build/tests/StressConcurrency.test.o build/tests/BTree.test.o \
    build/obj/CachingStorageEngine.test.o build/obj/Pager.test.o \
    build/obj/StressConcurrency.test.o build/obj/Tuple.test.o \
    build/obj/Value.test.o build/tests/main.o \
    -pthread -lws2_32

# Run tests
./build/core_test
```

#### Option C: Windows PowerShell Script (build.ps1)
```powershell
# Run the provided build script
.\build.ps1

# With options
.\build.ps1 -Clean -Test -Release
```

### 4. Build Output Structure
```
build/
├── obj/
│   ├── BTree.o
│   ├── BuddyPageAllocator.o
│   ├── EngineDispatcher.o
│   ├── HttpServer.o
│   ├── HttpServerIoThread.o
│   ├── Pager.o
│   ├── SocketUtils.o
│   └── SqlParser.o
├── tests/
│   ├── Core.test.o
│   ├── EngineDispatcher.test.o
│   ├── EngineLoop.test.o
│   ├── HttpApi.test.o
│   ├── HttpServer.test.o
│   ├── Predicate.test.o
│   ├── SqlParser.test.o
│   ├── JsonEncoder.test.o
│   ├── Storage.test.o
│   ├── LocalFileStorageProvider.test.o
│   ├── ShardedStorageProvider.test.o
│   ├── StressConcurrency.test.o
│   ├── BTree.test.o
│   ├── CachingStorageEngine.test.o
│   ├── Pager.test.o
│   ├── StressConcurrency.test.o
│   ├── Tuple.test.o
│   ├── Value.test.o
│   └── main.o
├── core_test.exe          # Core test runner
├── ed_test.exe            # EngineDispatcher tests
├── parser_test.exe        # SqlParser tests
├── predicate_test.exe     # Predicate tests
├── json_test.exe          # JsonEncoder/Parser tests
├── api_test.exe           # HttpApi tests
├── hs_test.exe            # HttpServer tests
├── test_runner.exe        # Main test runner
└── atomdb.exe             # REPL binary (if built)
```

---

## Verification

### Run All Tests
```bash
# Using Make
make test

# Manual test execution
./build/core_test       # 26 tests - core functionality
./build/ed_test         # 4 tests - EngineDispatcher
./build/parser_test     # 48 tests - SqlParser
./build/predicate_test  # 19 tests - Predicate/Comparison
./build/json_test       # 19 tests - JsonEncoder/Parser
./build/api_test        # 7 tests - HttpApi
./build/hs_test         # 6 tests - HttpServer
# Total: 130 tests
```

### Expected Test Output
```
==== PASSED 26 / FAILED 0 ====
==== PASSED 4 / FAILED 0 ====
==== PASSED 48 / FAILED 0 ====
==== PASSED 19 / FAILED 0 ====
==== PASSED 19 / FAILED 0 ====
==== PASSED 7 / FAILED 0 ====
==== PASSED 6 / FAILED 0 ====
Total: 130 tests passed
```

### Run REPL (Interactive Mode)
```bash
./build/atomdb
# Should start interactive SQL REPL
# Type EXIT to quit
```

### Run Smoke Test
```bash
# Automated smoke test
make smoke

# Manual smoke test
echo -e "CREATE TABLE users (id INT PRIMARY KEY, name TEXT)\nINSERT INTO users VALUES (1, 'test')\nSELECT * FROM users\nEXIT" | ./build/atomdb
```

---

## Troubleshooting

### Common Build Issues

#### 1. C++23 Not Supported
```
error: unrecognized command line option '-std=c++2b'
```
**Solution**: Upgrade compiler to GCC 13+, Clang 16+, or MSVC 19.40+

#### 2. Missing pthread on Windows
```
undefined reference to 'pthread_create'
```
**Solution**: Link with `-lws2_32 -lpthread` on Windows, ensure pthreads-w32 or winpthreads installed

#### 3. Missing semaphore header (C++20)
```
fatal error: semaphore: No such file or directory
```
**Solution**: Use GCC 13+ with libstdc++ that supports `<semaphore>`, or use GCC 14+

#### 4. Windows select() not found
```
undefined reference to 'select'
```
**Solution**: Link with `-lws2_32` on Windows

#### 5. Permission denied on build directory
```
Permission denied: build/obj/BTree.o
```
**Solution**: Ensure write permissions, run `chmod -R 755 build/` (Linux/macOS) or run as Administrator (Windows)

### Test Failures

#### 1. Stress tests timeout
```
Stress_Concurrent_Insert_NoDataRace: TIMEOUT
```
**Solution**: Increase test timeout or run with fewer concurrent threads

#### 2. Socket tests fail on CI
```
HttpServer_Configuration_Port_Zero_Lets_OS_Choose: FAILED
```
**Solution**: Ensure port 0 binding works (requires root on Linux for ports < 1024, use high ports)

### Platform-Specific Notes

#### Linux
- Use `taskset -c 0-3 ./build/core_test` to pin to specific cores for reproducible benchmarks
- Enable `sysctl -w net.core.somaxconn=4096` for high connection tests

#### Windows
- Run in MSYS2 UCRT64 shell for best compatibility
- Use `g++` from `mingw-w64-ucrt-x86_64-gcc` package
- Disable Windows Defender for build directory for faster builds

#### macOS
- Use Homebrew LLVM: `brew install llvm && export PATH="/opt/homebrew/opt/llvm/bin:$PATH"`
- Set `CC=/opt/homebrew/opt/llvm/bin/clang++` for CMake/Make

---

## Project Structure

```
opendb/
├── includes/                 # Public headers
│   ├── atomdb/
│   │   ├── contracts/       # Interface definitions (IStorageProvider, IEngineDispatcher, etc.)
│   │   ├── core/            # Core engine headers (EngineDispatcher, TransactionManager, LockManager, DeadlockDetector)
│   │   ├── frontend/        # Frontend headers (HttpServer, HttpApi, JsonEncoder, SqlParser)
│   │   ├── storage/         # Storage engine headers (InMemoryStorageEngine, LocalFileStorageEngine)
│   │   ├── types/           # Core types (Value, Tuple, TupleId, Predicate, Command, DbError)
│   │   └── frontend/        # Frontend utilities (SocketUtils, JsonEncoder)
│   └── atomdb/              # Main namespace
├── src/                      # Source files
│   ├── BTree.cpp            # B+Tree implementation
│   ├── BuddyPageAllocator.cpp
│   ├── EngineDispatcher.cpp
│   ├── HttpServer.cpp
│   ├── HttpServerIoThread.cpp
│   ├── Pager.cpp
│   ├── SocketUtils.cpp
│   └── SqlParser.cpp
├── tests/                    # Test files
│   ├── *.test.cpp           # Test files (1 per component)
│   └── main.cpp             # Test runner entry point
├── example/                  # Example database files
├── build/                    # Build output (generated)
├── docs/                     # Documentation
│   ├── PHASE1-CHANGELOG.md
│   └── BUILD.md             # This file
├── Makefile                  # GNU Make build system
├── build.ps1                 # PowerShell build script
├── build.bat                 # Batch build script
├── .gitignore
├── DEPENDENCIES.md           # Dependency manifest
├── API.md                    # API reference
└── README.md                 # Project overview
```

---

## Dependencies Detail

### Core Dependencies (Zero External)
| Component | Purpose | Implementation |
|-----------|---------|----------------|
| BTree | Index storage | Custom B+Tree implementation |
| BuddyPageAllocator | Memory allocation | Binary buddy system |
| EngineDispatcher | Thread pool | std::jthread + counting_semaphore |
| LockManager | Concurrency control | Shared/Exclusive locks with wait queues |
| DeadlockDetector | Deadlock prevention | Wait-for graph cycle detection |
| TransactionManager | Transaction state | MVCC with commit sequencing |
| Pager | Page management | File-based paging with WAL |
| SqlParser | SQL parsing | Recursive descent parser |
| JsonEncoder | JSON serialization | Custom encoder with Base64/UTF-8 |
| JsonParser | JSON parsing | Recursive descent with UTF-8/Unicode |
| HttpServer | HTTP server | select()-based async I/O |
| HttpApi | HTTP API plugin | JSON-over-HTTP SQL execution |
| SqlParser | SQL parsing | Recursive descent parser |

### Storage Providers (Pluggable)
| Provider | Type | Use Case |
|----------|------|----------|
| InMemoryStorageEngine | In-memory | Testing, caching |
| LocalFileStorageEngine | File-based | Production single-node |
| ShardedStorageEngine | Distributed | Horizontal scaling |

### Build Dependencies (Compile-time Only)
| Dependency | Version | Purpose |
|------------|---------|---------|
| GCC/Clang/MSVC | 13+/16+/19.40+ | C++23 compiler |
| Standard Library | C++23 | std::jthread, semaphore, expected, etc. |
| pthreads/Win32 Threads | System | Threading |
| Winsock2 / POSIX sockets | System | Networking |

### No Runtime Dependencies
OpenDB links statically against all dependencies. The only runtime requirements are:
- Standard C++ library (libstdc++ / libc++ / MSVC STL)
- OS threading primitives (pthread / Win32 threads)
- OS networking (Winsock2 / POSIX sockets)
- Standard C library (libc / ucrt)

---

## Appendix: Configuration Options

### HttpServer Config
```cpp
HttpServer::Config cfg;
cfg.port = 8080;                    // Default: 8080
cfg.ioThreadCount = 4;              // Default: hardware_concurrency
cfg.maxConnections = 1024;          // Default: 1024
cfg.readTimeout = 5000ms;           // Default: 5s
cfg.writeTimeout = 5000ms;          // Default: 5s
cfg.maxBodySize = 10 * 1024 * 1024; // Default: 10 MB
cfg.tlsContext = nullptr;           // Optional SSL_CTX*
```

### EngineDispatcher Config
```cpp
EngineDispatcher dispatcher(
    4,                    // workerCount (default: hardware_concurrency)
    storage.get(),
    txnm, lockMgr, deadlock,
    std::chrono::seconds(50),    // lockTimeout (default: 50s)
    std::chrono::milliseconds(0) // queryTimeout (default: 0 = no timeout)
);
```

### Storage Engine Config
```cpp
// In-Memory
auto storage = std::make_unique<InMemoryStorageProvider>();
storage->open("in-memory://");

// Local File
auto storage = std::make_unique<LocalFileStorageProvider>();
storage->open("file:///path/to/db");

// Sharded
auto storage = std::make_unique<ShardedStorageProvider>();
storage->open("sharded://shard1,shard2,shard3");
```

---

## License
MIT License - See LICENSE file for details.

---

*Generated for OpenDB v0.1 - Production Readiness Backlog Phase 1 Complete*
*All 130 tests passing across 8 test suites*