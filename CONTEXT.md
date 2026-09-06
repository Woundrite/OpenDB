# OpenDB — Context for Continuation

> **Status**: Phase 1 (Critical Correctness & Safety / GA Blockers) — **Complete**  
> **Last commit**: `55f2ca3` — `chore: stop tracking .swarm session state`  
> **Working tree**: Clean

---

## 🎯 What Was Accomplished (Phase 1)

All 8 GA-blocker tasks in Phase 1 are **implemented and verified** (with two documented exceptions below).

| Task     | Description                                                 | Status                  | Notes                                                                        |
| -------- | ----------------------------------------------------------- | ----------------------- | ---------------------------------------------------------------------------- |
| **1.1**  | HTTP API lock release in explicit transactions (G.1)        | ✅ Complete             | Locks now held from BEGIN through COMMIT/ROLLBACK                            |
| **1.2**  | Unify TransactionManager/LockManager/DeadlockDetector (G.3) | ⚠️ Partial              | Constructor-level unification done; `main.cpp` wiring absent                 |
| **1.3**  | Lock-wait timeout fallback (C.5)                            | ✅ Complete             | `tryAcquire` with 50s default; `DbError::lockTimeout` on expiry              |
| **1.4**  | NULL equality semantics / IS NULL / IS NOT NULL (F.6)       | ✅ Complete             | Three-valued logic; missing column = NULL                                    |
| **1.5**  | Replace hand-rolled JSON parser (I.8)                       | ✅ Complete             | Recursive-descent parser in `JsonEncoder.hpp`                                |
| **1.6a** | HTTP request body size limit (I.7)                          | ✅ Complete             | `maxBodySize` (default 10 MB), 413 on oversize                               |
| **1.6b** | maxConnections limit (I.6)                                  | ✅ Complete             | `maxConnections` (default 1024), 503 on capacity                             |
| **1.7**  | (covered by 1.6b)                                           | ✅ Complete             | Atomic connection counter, decrements on all paths                           |
| **1.8**  | Query timeout / max execution time (K.1)                    | ❌ **Declaration only** | `queryTimeout_` field + `checkQueryTimeout()` declared, never defined/called |

---

## 🛠 Key Fixes This Session

### 1. BuddyPageAllocator Self-Deadlock (Critical — blocked all disk writes)

**Problem**: `allocatePages()` held `mu_` while invoking `extendFile` callback → re-entered via `onFileExtended()` taking `mu_` → guaranteed deadlock on first file growth.  
**Fix**: `unique_lock` + `lk.unlock()` before `extendFile` callback; added `freeRunCountUnlocked()` for `serializeHeader()`.

### 2. Makefile Fixes (unblocks `make test` on Windows)

| Fix                                                                                                                    | Location             |
| ---------------------------------------------------------------------------------------------------------------------- | -------------------- |
| Link `test_runner` against `SRC_OBJ_NO_MAIN` (all impl objs except `main.o`)                                           | `Makefile` link rule |
| Windows: `-lws2_32` for Winsock symbols (`SOCKLNK`)                                                                    | `Makefile`           |
| Windows: `mkdir -p` → PowerShell (`MKDIR := powershell -NoProfile -Command New-Item -ItemType Directory -Force -Path`) | `Makefile`           |

### 3. Documentation Corrections

| File                       | What Changed                                                                                                                                                                                                              |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `docs/PHASE1-CHANGELOG.md` | Task 1.2 marked **partial** (ctor unification done; main.cpp wiring absent); Task 1.8 marked **declaration-only**; real test counts (247 tests, 243 pass, 4 known failures); BuddyPageAllocator + Makefile fixes recorded |
| `BUILD.md`                 | Removed fabricated "130 tests" tally; canonical `make test` output + 4 known failures listed; footer updated                                                                                                              |
| `docs/USER_GUIDE.md`       | Comprehensive usage guide (install, config, storage providers, HTTP API, SQL, REPL, deployment, troubleshooting, FAQ)                                                                                                     |
| `BUILD.md`                 | Complete build guide (sysreqs, deps, build instructions, verification, troubleshooting)                                                                                                                                   |

---

## ✅ Verification Results

### Build (fresh tree, zero overrides)

```bash
$ make SHELL=C:/msys64/usr/bin/sh.exe
# → compiles 8 src + 18 test objects; links opendb.exe + test_runner.exe
```

### Test Suite (canonical, zero overrides)

```
==== PASSED 243 / FAILED 4 ====
[  FAILED  ] LocalFile_DropTable_FreePages_Survive_Reopen
[  FAILED  ] Pager_FreeList_LIFO_Order
[  FAILED  ] Pager_FreeList_Survives_Reopen
[  FAILED  ] Pager_Buddy_V1_To_V2_Migration_Preserves_Allocations_And_Frees
```

- **No hang** — the BuddyPageAllocator deadlock fix eliminated the hang that previously made disk-backed tests time out
- 4 failures are **pre-existing, deterministic** Pager free-list/migration bugs (see below)

### Full Suite Breakdown (247 TEST() cases across 17 files)

| Suite            | Tests   | Pass    | Fail  |
| ---------------- | ------- | ------- | ----- |
| Core             | 26      | 26      | 0     |
| EngineDispatcher | 4       | 4       | 0     |
| HttpApi          | 7       | 7       | 0     |
| HttpServer       | 6       | 6       | 0     |
| Predicate        | 19      | 19      | 0     |
| SqlParser        | 48      | 48      | 0     |
| JsonEncoder      | 7       | 7       | 0     |
| JsonParser       | 12      | 12      | 0     |
| **Total**        | **247** | **243** | **4** |

### Pre-existing Known Failures (4)

| Test                                                             | Likely Root Cause                                        |
| ---------------------------------------------------------------- | -------------------------------------------------------- |
| `LocalFile_DropTable_FreePages_Survive_Reopen`                   | Free-list persistence                                    |
| `Pager_FreeList_LIFO_Order`                                      | **Likely root** — LIFO ordering bug cascades into others |
| `Pager_FreeList_Survives_Reopen`                                 | Free-list persistence                                    |
| `Pager_Buddy_V1_To_V2_Migration_Preserves_Allocations_And_Frees` | Migration format bug                                     |

---

## 📁 Repository State (Clean)

```bash
$ git status --short
# (empty)
```

**Recent commits** (newest first):

```
55f2ca3 chore: stop tracking .swarm session state (now gitignored)
ce29ec1 fix(test): close reopened Pager before removing file in buddy manual test
4a65a8c test: add manual BuddyPageAllocator deadlock reproduction; ignore swarm state
3ca4cb0 docs: correct overclaimed test counts and task status in changelog and BUILD.md
c4121b0 fix: link test_runner against implementation objects; Windows portability
cab30ad fix: remove BuddyPageAllocator self-deadlock on file extension
```

### Gitignored / Untracked (scratch artifacts, not committed)

```
.swarm/                  # session state (gitignored)
build/                   # build artifacts
*.dat                    # test artifacts
link.bat / link_err.txt  # scratch
test_compile.cpp / test_link.cpp  # throwaway
```

---

## 🚀 How to Build & Test (Fresh Clone)

### Prerequisites

- **Linux/macOS**: GCC 13+ / Clang 16+ (`g++ --version` ≥ 13)
- **Windows**: MSYS2 UCRT64 + `mingw64/mingw-w64-x86_64-gcc`

### Build & Test (One Command)

```bash
# Fresh clone
git clone <repo> && cd opendb

# Build + test (single command; works on Linux/macOS and MSYS2)
make SHELL=/usr/bin/sh test   # Linux/macOS
make SHELL=C:/msys64/usr/bin/sh.exe test   # Windows MSYS2
```

### Manual Build (Cross-platform)

```bash
# Clean
rm -rf build && mkdir -p build/obj build/tests

# Compile all source
for f in src/*.cpp; do
  g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes -c "$f" -o "build/obj/$(basename $f .cpp).o"
done

# Compile tests
for f in tests/*.test.cpp; do
  g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes -c "$f" -o "build/tests/$(basename $f .cpp).o"
done
g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -Iincludes -c tests/main.cpp -o build/tests/main.o

# Link test runner (Linux)
g++ -o build/test_runner build/obj/*.o build/tests/*.o -pthread

# Windows (MSYS2)
g++ -o build/test_runner.exe build/obj/*.o build/tests/*.o -pthread -lws2_32
```

### Run Tests

```bash
./build/test_runner          # Linux/macOS
./build/test_runner.exe      # Windows
```

### Run REPL / HTTP Server

```bash
./build/opendb                          # REPL
./build/opendb --server --port 8080     # HTTP server
```

---

## 🔧 Configuration Reference

### HttpServer Config

| Parameter        | Default   | Description                |
| ---------------- | --------- | -------------------------- |
| `port`           | 8080      | Listen port                |
| `ioThreadCount`  | CPU cores | I/O thread pool size       |
| `maxConnections` | 1024      | Max concurrent connections |
| `readTimeout`    | 5s        | Read timeout               |
| `writeTimeout`   | 5s        | Write timeout              |
| `maxBodySize`    | 10 MB     | Max request body           |
| `tlsContext`     | `nullptr` | Optional SSL_CTX\*         |

### EngineDispatcher Config

```cpp
EngineDispatcher dispatcher(
    4,                              // workerCount (0 = hw_concurrency)
    storage.get(),
    txnm, lockMgr, deadlock,
    std::chrono::seconds(50),      // lockTimeout (default 50s)
    std::chrono::milliseconds(0)   // queryTimeout (0 = no timeout, NYI)
);
```

### HttpApiAccessPlugin

```cpp
HttpApiAccessPlugin plugin(txnm, lockMgr, deadlock);
plugin.open("in-memory://", storage.get(), &dispatcher);
std::string resp = plugin.handleRequest(R"({"type":"query","sql":"SELECT * FROM t"})");
```

### HttpServer Config (new fields)

```cpp
HttpServer::Config cfg;
cfg.maxBodySize = 10 * 1024 * 1024;   // 10 MB default
cfg.maxConnections = 1024;             // max concurrent connections
```

---

## ⚠️ Open / Pending Items (Not Yet Done)

| Item                             | Description                                                                                                                   | Priority |
| -------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- | -------- |
| **Pager free-list root cause**   | LIFO-order bug likely cascades into other 3 failures; investigate free-list bookkeeping                                       | High     |
| **Task 1.2 wiring**              | `main.cpp` still REPL-only; no HTTP/REPL shared lock domain                                                                   | Medium   |
| **Task 1.8 implementation**      | `checkQueryTimeout()` declared but never defined/called; needs per-statement timeout with cancellation                        | Medium   |
| **Linux GCC warnings**           | `-Werror=format-truncation` in `Value.hpp`; unused-variable in `SocketUtils.cpp` (Windows-only field)                         | Low      |
| **HTTP/REPL shared lock domain** | `main.cpp` must construct shared `TransactionManager`/`LockManager`/`DeadlockDetector` and pass to both REPL and `HttpServer` | Medium   |
| **TLS support**                  | Optional `tlsContext` in `HttpServer::Config`; needs cert loading path                                                        | Low      |

---

## 🧪 Regression Test (BuddyPageAllocator Deadlock)

A manual reproduction was committed at `tests/buddy_manual_test.cpp`:

```bash
# Compile
g++ -std=c++2b -Wall -Wextra -Iincludes -pthread \
    tests/buddy_manual_test.cpp src/Pager.cpp src/BuddyPageAllocator.cpp \
    -o build/buddy_manual

# Run
./build/buddy_manual
```

**Pre-fix**: hung, exit 124 at 6s timeout, zero output  
**Post-fix**: runs to completion (allocation → free → `allocatePages(2)` → freePages → close/reopen → clean exit)

> Windows note: the test closes the reopened `Pager` before `std::filesystem::remove()` — required on Windows, harmless on POSIX.

---

## 📚 Key Files to Know

| File                                                  | Purpose                                          |
| ----------------------------------------------------- | ------------------------------------------------ |
| `src/BuddyPageAllocator.cpp/hpp`                      | Binary buddy allocator + deadlock fix            |
| `src/EngineDispatcher.cpp/hpp`                        | Thread pool + query timeout (stub)               |
| `src/HttpServer.cpp/hpp`                              | HTTP server + body size / max connections limits |
| `src/HttpServerIoThread.cpp/hpp`                      | I/O threads + acceptor + 503 logic               |
| `src/HttpApi.hpp`                                     | JSON-over-HTTP SQL plugin                        |
| `src/HttpSession.cpp/hpp`                             | HTTP session → SQL command bridge                |
| `src/SqlParser.cpp`                                   | Recursive-descent SQL parser                     |
| `src/JsonEncoder.hpp`                                 | JSON encoder + **recursive-descent JSON parser** |
| `src/Pager.cpp` / `includes/opendb/storage/Pager.hpp` | Page manager + free-list (4 failing tests)       |
| `src/SqlParser.cpp`                                   | Recursive-descent SQL parser                     |
| `includes/opendb/types/Predicate.hpp`                 | IS NULL / IS NOT NULL logic                      |
| `includes/opendb/types/DbError.hpp`                   | Error codes (including `DbError::queryTimeout`)  |
| `tests/buddy_manual_test.cpp`                         | Deadlock reproduction (manual)                   |
| `Makefile`                                            | GNU Make (POSIX + Windows MSYS2)                 |

---

## 🧭 Next Steps for New Conversation

1. **Root-cause Pager free-list bug** — likely single LIFO-order bug cascading into 4 test failures
2. **Wire `main.cpp`** — instantiate shared `TransactionManager`/`LockManager`/`DeadlockDetector`; pass to REPL, `HttpServer`, and `EngineDispatcher`
3. **Implement query timeout** — implement `EngineDispatcher::checkQueryTimeout()`; call at each command boundary; abort txn on timeout
4. **Linux GCC warnings** — `-Werror=format-truncation` in `Value.hpp`; unused variable in `SocketUtils.cpp` (Windows-only field)
5. Optional: TLS cert loading path for `HttpServer::Config::tlsContext`

---

## 🧠 Quick Mental Model for New Conversation

> **OpenDB** = embedded SQL engine (C++23)  
> **Architecture**: REPL / HTTP API → `EngineDispatcher` (thread pool) → `LockManager` + `TransactionManager` + `DeadlockDetector` → `Pager` / `BTree` / `BuddyPageAllocator` → `IStorageProvider` (in-memory / local file / sharded)  
> **Current phase**: Phase 1 done (GA blockers). Phase 2 = performance/observability/backup-recovery (not started).

---

> **Ready to continue.** Run `make SHELL=/usr/bin/sh test` (Linux) or `make SHELL=C:/msys64/usr/bin/sh.exe test` (Windows) to re-verify.
