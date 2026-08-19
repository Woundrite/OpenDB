# OpenDB User Guide

## Overview
OpenDB is an embedded SQL database engine written in C++23, designed for high-performance embedded workloads. It provides a lightweight, zero-dependency database solution with support for multiple storage backends, HTTP API access, and an interactive REPL.

## Table of Contents
1. [Quick Start](#quick-start)
2. [Installation](#installation)
3. [Running OpenDB](#running-opendb)
4. [Configuration](#configuration)
5. [Storage Providers](#storage-providers)
6. [HTTP API](#http-api)
7. [SQL Reference](#sql-reference)
8. [REPL Usage](#repl-usage)
9. [Deployment](#deployment)
10. [Troubleshooting](#troubleshooting)

---

## Quick Start

### Prerequisites
- C++23 compatible compiler (GCC 13+, Clang 16+, MSVC 19.40+)
- Standard C++ library with C++23 support
- POSIX threads (Linux/macOS) or Win32 threads (Windows)

### Build and Run in 3 Steps
```bash
# 1. Clone and build
git clone https://github.com/your-org/opendb.git
cd opendb
make

# 2. Start REPL
./build/atomdb

# 3. Run SQL
opendb> CREATE TABLE users (id INT PRIMARY KEY, name TEXT, email TEXT);
opendb> INSERT INTO users VALUES (1, 'Alice', 'alice@example.com');
opendb> SELECT * FROM users;
```

---

## Installation

### From Source (Recommended)
```bash
# Clone repository
git clone https://github.com/your-org/opendb.git
cd opendb

# Build with Make (Linux/macOS/Windows MSYS2)
make

# Or use PowerShell on Windows
.\build.ps1

# Run tests to verify
make test
```

### Pre-built Binaries
Currently, OpenDB is distributed as source only. See [BUILD.md](BUILD.md) for detailed build instructions.

---

## Running OpenDB

### REPL Mode (Interactive)
```bash
./build/atomdb
```
Starts an interactive SQL shell. Type `EXIT` or press `Ctrl+D` to quit.

### HTTP Server Mode
```bash
# Start HTTP server on port 8080
./build/atomdb --server --port 8080

# With custom config
./build/atomdb --server --port 8080 --max-connections 500 --max-body-size 20MB
```

### Embedded Library Mode
```cpp
#include "atomdb/frontend/HttpApi.hpp"
#include "atomdb/storage/InMemoryStorageProvider.hpp"
#include "atomdb/core/EngineDispatcher.hpp"

using namespace atomdb;

int main() {
    // 1. Create storage provider
    auto storage = std::make_unique<InMemoryStorageProvider>();
    storage->open("in-memory://");

    // 2. Create core components
    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector deadlock(lockMgr);

    // 3. Create dispatcher (4 worker threads)
    EngineDispatcher dispatcher(4, storage.get(), txnm, lockMgr, deadlock,
                               std::chrono::seconds(50),  // lock timeout
                               std::chrono::milliseconds(0)); // no query timeout

    // 4. Create HTTP API plugin
    HttpApiAccessPlugin plugin(txnm, lockMgr, deadlock);
    plugin.open("in-memory://", storage.get(), &dispatcher);

    // 5. Execute SQL via JSON
    std::string request = R"({"type":"query","sql":"CREATE TABLE t (id INT PRIMARY KEY, name TEXT)"})";
    std::string response = plugin.handleRequest(request);
    // {"success":true}

    request = R"({"type":"query","sql":"INSERT INTO t VALUES (1, 'hello')"})";
    response = plugin.handleRequest(request);
    // {"success":true}

    request = R"({"type":"query","sql":"SELECT * FROM t"})";
    response = plugin.handleRequest(request);
    // {"success":true,"rows":[{"id":1,"name":"hello"}]}

    return 0;
}
```

---

## Configuration

### HttpServer Configuration
```cpp
HttpServer::Config cfg;
cfg.port = 8080;                          // Default: 8080
cfg.ioThreadCount = 4;                    // Default: hardware_concurrency
cfg.maxConnections = 1024;                // Default: 1024
cfg.readTimeout = std::chrono::seconds(5);   // Default: 5s
cfg.writeTimeout = std::chrono::seconds(5);  // Default: 5s
cfg.maxBodySize = 10 * 1024 * 1024;      // Default: 10 MB
cfg.tlsContext = nullptr;                 // Optional SSL_CTX*
```

### EngineDispatcher Configuration
```cpp
EngineDispatcher dispatcher(
    4,                                      // workerCount (0 = hardware_concurrency)
    storage.get(),
    txnm, lockMgr, deadlock,
    std::chrono::seconds(50),              // lockTimeout (default: 50s)
    std::chrono::milliseconds(0)            // queryTimeout (0 = no timeout)
);
```

### HttpApiAccessPlugin Configuration
```cpp
HttpApiAccessPlugin plugin(txnm, lockMgr, deadlock);
// Uses shared TransactionManager, LockManager, DeadlockDetector
plugin.open("in-memory://", storage.get(), &dispatcher);
```

### Environment Variables
| Variable | Description | Default |
|----------|-------------|---------|
| `OPENDB_PORT` | HTTP server port | 8080 |
| `OPENDB_MAX_CONNECTIONS` | Max concurrent connections | 1024 |
| `OPENDB_MAX_BODY_SIZE` | Max request body size (bytes) | 10485760 (10MB) |
| `OPENDB_LOCK_TIMEOUT` | Lock wait timeout (ms) | 50000 |
| `OPENDB_QUERY_TIMEOUT` | Query execution timeout (ms) | 0 (disabled) |
| `OPENDB_THREADS` | Worker thread count | CPU cores |

---

## Storage Providers

OpenDB supports multiple pluggable storage backends. Select at runtime by URI scheme.

### 1. In-Memory Storage (`in-memory://`)
```cpp
auto storage = std::make_unique<InMemoryStorageProvider>();
storage->open("in-memory://");
```
**Use cases**: Testing, caching, ephemeral data, unit tests
**Characteristics**: 
- Zero persistence
- Fastest performance
- Data lost on process exit
- No file I/O overhead

### 2. Local File Storage (`file://`)
```cpp
auto storage = std::make_unique<LocalFileStorageProvider>();
storage->open("file:///var/lib/opendb/mydb");
```
**Use cases**: Production single-node, development, embedded devices
**Characteristics**:
- Full persistence with WAL
- ACID transactions
- Page-based storage with B+Tree indexes
- Configurable page size (default 4KB)

### 3. Sharded Storage (`sharded://`)
```cpp
auto storage = std::make_unique<ShardedStorageProvider>();
storage->open("sharded://shard1,shard2,shard3");
```
**Use cases**: Horizontal scaling, multi-tenant, high availability
**Characteristics**:
- Hash-based sharding (configurable)
- Automatic shard routing
- Cross-shard transactions (limited)
- Per-shard storage providers

### Custom Storage Provider
Implement `IStorageProvider` interface:
```cpp
class CustomStorageProvider : public IStorageProvider {
public:
    DbError open(const std::string& uri) override;
    DbError close() override;
    DbError createTable(const Schema& schema) override;
    DbError dropTable(const std::string& table) override;
    std::optional<TableDescriptor> describeTable(const std::string& table) override;
    std::vector<std::string> tables() const override;
    IStorageEngine* engine() override;
};
```

---

## HTTP API

### Base URL
```
http://localhost:8080
```

### Endpoints

#### POST /query
Execute SQL query
```bash
curl -X POST http://localhost:8080/query \
  -H "Content-Type: application/json" \
  -d '{"type":"query","sql":"SELECT * FROM users WHERE age > 25"}'
```

#### POST /begin
Begin transaction
```bash
curl -X POST http://localhost:8080/begin
# Returns: {"success":true,"txnId":123}
```

#### POST /commit
Commit transaction
```bash
curl -X POST http://localhost:8080/commit \
  -H "Content-Type: application/json" \
  -d '{"type":"commit","txnId":123}'
```

#### POST /rollback
Rollback transaction
```bash
curl -X POST http://localhost:8080/rollback \
  -H "Content-Type: application/json" \
  -d '{"type":"rollback","txnId":123}'
```

#### GET /health
Health check
```bash
curl http://localhost:8080/health
# {"status":"ok"}
```

#### GET /metrics
Prometheus-compatible metrics
```bash
curl http://localhost:8080/metrics
# {
#   "http":{"connections_accepted":10,"connections_active":2,...},
#   "engine":{"sessions_enqueued":100,"sessions_completed":98,...}
# }
```

### Request Format
```json
{
  "type": "query|begin|commit|rollback",
  "sql": "SQL statement (required for query)",
  "txnId": 123 (optional, for explicit transactions)
}
```

### Response Format
```json
// Success
{"success":true,"rows":[...],"txnId":123}

// Error
{"success":false,"error":"error message"}

// Begin response
{"success":true,"txnId":123}
```

### Error Codes
| HTTP Status | Error Code | Description |
|-------------|------------|-------------|
| 200 | success | Request succeeded |
| 400 | parseError | Invalid JSON or SQL syntax |
| 404 | notFound | Table/row not found |
| 409 | deadlock/conflict | Transaction conflict |
| 413 | payloadTooLarge | Request body exceeds maxBodySize |
| 500 | internal | Internal server error |
| 503 | serviceUnavailable | Server at max connections |

---

## SQL Reference

### Data Types
| Type | Description | Range |
|------|-------------|-------|
| `INT` / `INT32` | 32-bit signed integer | -2^31 to 2^31-1 |
| `BIGINT` / `INT64` | 64-bit signed integer | -2^63 to 2^63-1 |
| `DOUBLE` / `REAL` / `FLOAT` | 64-bit floating point | IEEE 754 double |
| `TEXT` / `VARCHAR` / `CHAR` | Variable-length text | Up to 1GB |
| `BLOB` / `BYTEA` | Binary data | Up to 1GB |
| `BOOL` / `BOOLEAN` | Boolean | true/false |
| `DATE` | Calendar date | YYYY-MM-DD |
| `TIMESTAMP` | Date + time | YYYY-MM-DD HH:MM:SS.mmm |

### DDL Statements

#### CREATE TABLE
```sql
CREATE TABLE users (
    id INT PRIMARY KEY,
    name TEXT NOT NULL,
    email TEXT UNIQUE,
    age INT DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

#### DROP TABLE
```sql
DROP TABLE users;
```

#### ALTER TABLE
```sql
-- Add column
ALTER TABLE users ADD COLUMN phone TEXT;

-- Drop column
ALTER TABLE users DROP COLUMN phone;

-- Rename table
ALTER TABLE users RENAME TO customers;

-- Add index
ALTER TABLE users ADD INDEX idx_email (email);
```

### DML Statements

#### INSERT
```sql
-- Explicit values
INSERT INTO users (id, name, email) VALUES (1, 'Alice', 'alice@example.com');

-- Default values
INSERT INTO users DEFAULT VALUES;

-- Multiple rows
INSERT INTO users (id, name) VALUES (1, 'Alice'), (2, 'Bob');
```

#### SELECT
```sql
-- Basic select
SELECT * FROM users;
SELECT id, name FROM users WHERE age > 25;
SELECT * FROM users ORDER BY name ASC LIMIT 10 OFFSET 5;

-- Aggregations
SELECT COUNT(*) FROM users;
SELECT AVG(age), MAX(age) FROM users GROUP BY department;

-- Joins
SELECT u.name, o.total FROM users u
INNER JOIN orders o ON u.id = o.user_id
WHERE u.age > 25;
```

#### UPDATE
```sql
UPDATE users SET age = 26 WHERE id = 1;
UPDATE users SET age = age + 1 WHERE department = 'engineering';
```

#### DELETE
```sql
DELETE FROM users WHERE id = 1;
DELETE FROM users WHERE age < 18;
```

### Transaction Control
```sql
BEGIN;
UPDATE accounts SET balance = balance - 100 WHERE id = 1;
UPDATE accounts SET balance = balance + 100 WHERE id = 2;
COMMIT;

-- Or rollback
ROLLBACK;
```

### Expressions and Operators

#### Comparison Operators
| Operator | Description |
|----------|-------------|
| `=` | Equal |
| `!=` / `<>` | Not equal |
| `<` | Less than |
| `<=` | Less than or equal |
| `>` | Greater than |
| `>=` | Greater than or equal |
| `IS NULL` | Null check |
| `IS NOT NULL` | Not null check |

#### Logical Operators
| Operator | Description |
|----------|-------------|
| `AND` | Logical AND |
| `OR` | Logical OR |
| `NOT` | Logical NOT |

#### Arithmetic Operators
| Operator | Description |
|----------|-------------|
| `+` | Addition |
| `-` | Subtraction |
| `*` | Multiplication |
| `/` | Division |

### Three-Valued Logic (NULL Semantics)
```sql
-- NULL = NULL → UNKNOWN (not true, not false)
-- NULL != NULL → UNKNOWN
-- NULL IS NULL → TRUE
-- NULL IS NOT NULL → FALSE
-- 5 = NULL → UNKNOWN
-- 5 != NULL → UNKNOWN

-- WHERE clause treats UNKNOWN as FALSE
SELECT * FROM t WHERE x = NULL;     -- Returns 0 rows
SELECT * FROM t WHERE x IS NULL;    -- Returns rows where x is NULL
SELECT * FROM t WHERE x IS NOT NULL; -- Returns rows where x is not NULL
```

---

## REPL Usage

### Starting REPL
```bash
./build/atomdb
```

### REPL Commands
| Command | Description |
|---------|-------------|
| `EXIT` / `QUIT` | Exit REPL |
| `\help` | Show help |
| `\tables` | List all tables |
| `\schema <table>` | Show table schema |
| `\indexes <table>` | List indexes |
| `\timing on/off` | Toggle query timing |
| `\mode json/csv/table` | Set output format |

### Example Session
```
opendb> CREATE TABLE products (id INT PRIMARY KEY, name TEXT, price DOUBLE);
opendb> INSERT INTO products VALUES (1, 'Widget', 19.99);
opendb> INSERT INTO products VALUES (2, 'Gadget', 29.99);
opendb> SELECT * FROM products WHERE price < 25;
+----+--------+-------+
| id | name   | price |
+----+--------+-------+
| 1  | Widget | 19.99 |
+----+--------+-------+
opendb> \tables
products
opendb> \schema products
+--------+--------+-----------+----------+---------+
| column | type   | not_null  | pk       | default |
+--------+--------+-----------+----------+---------+
| id     | INT    | YES       | YES      |         |
| name   | TEXT   | YES       | NO       |         |
| price  | DOUBLE | NO        | NO       |         |
+--------+--------+-----------+----------+---------+
opendb> EXIT
```

---

## Deployment

### Production Checklist
- [ ] Build in Release mode (`make RELEASE=1`)
- [ ] Configure appropriate `maxConnections` for expected load
- [ ] Set `maxBodySize` based on expected request sizes
- [ ] Configure `lockTimeout` and `queryTimeout` for your workload
- [ ] Set up TLS for production HTTP endpoints
- [ ] Configure appropriate storage provider (LocalFile/Sharded)
- [ ] Set up backup strategy for file-based storage
- [ ] Configure monitoring via `/metrics` endpoint
- [ ] Set up log rotation for HTTP access logs

### Docker Deployment
```dockerfile
FROM gcc:14 AS builder
WORKDIR /app
COPY . .
RUN make RELEASE=1

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y libstdc++6 && rm -rf /var/lib/apt/lists/*
COPY --from=builder /app/build/atomdb /usr/local/bin/atomdb
COPY --from=builder /app/build/atomdb /usr/local/bin/atomdb-server
ENTRYPOINT ["atomdb-server"]
EXPOSE 8080
```

### Systemd Service (Linux)
```ini
# /etc/systemd/system/opendb.service
[Unit]
Description=OpenDB HTTP Server
After=network.target

[Service]
Type=simple
User=opendb
WorkingDirectory=/opt/opendb
ExecStart=/opt/opendb/build/atomdb --server --port 8080
Restart=on-failure
RestartSec=5
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

### Kubernetes Deployment
```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: opendb
spec:
  replicas: 3
  selector:
    matchLabels:
      app: opendb
  template:
    metadata:
      labels:
        app: opendb
    spec:
      containers:
      - name: opendb
        image: opendb:latest
        ports:
        - containerPort: 8080
        env:
        - name: OPENDB_MAX_CONNECTIONS
          value: "500"
        - name: OPENDB_MAX_BODY_SIZE
          value: "10485760"
        resources:
          requests:
            memory: "256Mi"
            cpu: "250m"
          limits:
            memory: "512Mi"
            cpu: "1000m"
        livenessProbe:
          httpGet:
            path: /health
            port: 8080
          initialDelaySeconds: 10
          periodSeconds: 30
---
apiVersion: v1
kind: Service
metadata:
  name: opendb
spec:
  selector:
    app: opendb
  ports:
  - port: 8080
    targetPort: 8080
  type: ClusterIP
```

---

## Troubleshooting

### Connection Issues

#### "Connection refused" on HTTP endpoint
- Verify server is running: `ps aux | grep atomdb`
- Check port binding: `netstat -tlnp | grep 8080`
- Check firewall: `ufw status` or `iptables -L`

#### "503 Service Unavailable"
- Server at max connections (`maxConnections` reached)
- Check `stats.connections_active` via `/metrics`

#### "413 Payload Too Large"
- Request body exceeds `maxBodySize` (default 10MB)
- Increase `maxBodySize` in config or compress request

### Performance Issues

#### High Latency
- Check `/metrics` for `max_latency_us`
- Increase `ioThreadCount` for more concurrent I/O
- Ensure storage is on fast storage (SSD/NVMe)

#### High Memory Usage
- Reduce `maxConnections`
- Decrease `maxBodySize`
- Check for memory leaks in custom storage providers

#### Lock Contention
- Increase `lockTimeout` if legitimate long transactions
- Check for missing indexes causing full table scans
- Monitor `requests_conflicted` metric

### Data Issues

#### "Lock timeout" errors
- Increase `lockTimeout` (default 50s)
- Check for long-running uncommitted transactions
- Ensure proper transaction boundaries (COMMIT/ROLLBACK)

#### "Deadlock detected"
- Ensure consistent table access order across transactions
- Keep transactions short
- Use `SELECT ... FOR UPDATE` for explicit locking

#### Data Corruption
- Verify storage disk health: `smartctl -a /dev/sdX`
- Check filesystem integrity: `fsck`
- Restore from backup if WAL corrupted

### Debugging

#### Enable Debug Logging
```cpp
// Set log level at startup
std::setenv("OPENDB_LOG_LEVEL", "DEBUG", 1);
```

#### Metrics Endpoint
```bash
curl http://localhost:8080/metrics | jq .
```

#### REPL Debugging
```
opendb> \timing on
opendb> EXPLAIN SELECT * FROM users WHERE age > 25;
```

---

## Performance Tuning

### Connection Pooling
```cpp
// Reuse HTTP connections
curl --http1.1 --keepalive-time 60 http://localhost:8080/query ...
```

### Batch Operations
```sql
-- Instead of multiple INSERTs
INSERT INTO users (id, name) VALUES (1, 'A'), (2, 'B'), (3, 'C');

-- Use transactions for bulk updates
BEGIN;
UPDATE accounts SET balance = balance - 100 WHERE id IN (1,2,3);
UPDATE accounts SET balance = balance + 100 WHERE id IN (4,5,6);
COMMIT;
```

### Indexing Strategy
```sql
-- Create indexes for WHERE/JOIN columns
CREATE INDEX idx_users_email ON users(email);
CREATE INDEX idx_orders_user_date ON orders(user_id, created_at);

-- Avoid over-indexing (slows writes)
-- Monitor index usage via \indexes command
```

### Storage Tuning
```cpp
// Larger page size for large records
storage->open("file:///data/db?page_size=16384");

// Disable sync for testing (DANGEROUS in production)
storage->open("file:///data/db?sync=false");
```

---

## Security

### TLS/SSL Setup
```cpp
// Generate certificates
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes

// Configure HttpServer
SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM);
SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM);

HttpServer::Config cfg;
cfg.tlsContext = ctx;
```

### Authentication
OpenDB does not include built-in authentication. Implement at application layer:
```cpp
// Custom middleware in HttpApiAccessPlugin
std::string handleRequest(const std::string& requestJson) {
    // Parse request
    auto req = parseJsonRequest(requestJson);
    
    // Check auth header (implement your logic)
    if (!validateAuth(req)) {
        return JsonEncoder::encode(DbError::notSupported("unauthorized"));
    }
    
    return executeRequest(*req);
}
```

### Input Validation
- All SQL goes through parameterized parser (no injection via parser)
- HTTP body size limited by `maxBodySize`
- SQL length limited by parser (configurable)

---

## Migration Guide

### From SQLite
```sql
-- SQLite: AUTOINCREMENT
-- OpenDB: Use manual ID or MAX(id)+1
CREATE TABLE t (id INT PRIMARY KEY, ...);
INSERT INTO t (id, ...) SELECT COALESCE(MAX(id), 0) + 1, ... FROM t;

-- SQLite: || for concatenation
-- OpenDB: Use || (supported)
SELECT first_name || ' ' || last_name FROM users;
```

### From PostgreSQL/MySQL
```sql
-- SERIAL/AUTO_INCREMENT → Manual ID
-- Use MAX(id)+1 or application-generated IDs

-- TIMESTAMP → Use TIMESTAMP type (microsecond precision)
-- UUID → Store as TEXT (36 chars) or BLOB (16 bytes)

-- JSONB → Store as TEXT, parse in application
```

---

## FAQ

### Q: Does OpenDB support replication?
A: Not built-in. Use ShardedStorageProvider for horizontal scaling, or implement application-level replication.

### Q: What is the maximum database size?
A: Limited by filesystem (2^63 bytes per file). Practical limits depend on available disk space and memory.

### Q: Can I use OpenDB in a multi-threaded application?
A: Yes! EngineDispatcher is thread-safe. Each thread can enqueue sessions concurrently.

### Q: How do I backup?
- In-Memory: Not persistent (export via SELECT)
- LocalFile: Copy database directory (ensure no active writers or use WAL checkpoint)
- Sharded: Backup each shard independently

### Q: Does OpenDB support foreign keys?
A: Not currently. Enforce referential integrity at application layer.

### Q: What is the maximum connections?
A: Default 1024, configurable via `maxConnections`. Limited by OS file descriptor limit.

---

## Support

### Resources
- GitHub Issues: https://github.com/your-org/opendb/issues
- API Reference: [API.md](API.md)
- Build Guide: [BUILD.md](BUILD.md)
- Changelog: [docs/PHASE1-CHANGELOG.md](docs/PHASE1-CHANGELOG.md)

### Contributing
See CONTRIBUTING.md for development setup, coding standards, and PR process.

---

*OpenDB v0.1 - Embedded SQL Database Engine*
*Phase 1: Critical Correctness & Safety (GA Blockers) - Complete*