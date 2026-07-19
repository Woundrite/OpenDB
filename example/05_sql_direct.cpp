// example/05_sql_direct.cpp
//
// SQLite-style direct SQL interface. Type real SQL (CREATE, INSERT, SELECT,
// UPDATE, DELETE, etc.) and see results on stdout.
//
// |
// |  Supported (subset of spec §4.2):
// |    CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT)
// |    CREATE TABLE t (id INT PK, ...) PARTITION BY HASH(id) PARTITIONS 4
// |    DROP TABLE users
// |    INSERT INTO users VALUES (1, 'nikhil', 30)
// |    INSERT INTO users (id, name, age) VALUES (2, 'alice', 25)
// |    SELECT id, name FROM users WHERE age > 25
// |    UPDATE users SET age = 31 WHERE id = 1
// |    DELETE FROM users WHERE id = 1
// |    BEGIN   COMMIT   ROLLBACK      (sentinels; auto-commit in this example)
// |
// |  How it differs from a production SQL interface:
// |    - No multi-statement transactions; every DML is auto-commit.
// |    - Reads from stdin; statements separated by newlines (optional ';').
// |    - Errors print in-line but the loop keeps going (per spec §5.4).
// |    - UPDATE/DELETE use direct engine access (bypasses EngineLoop locking).
// |
// |  Storage: in-memory by default, LocalFile if first argv is file:// URI.
// |
// |  Compile:
// |    g++ -std=c++2b -Wall -Wextra -Wpedantic -Iincludes -pthread \
// |         src/SqlParser.cpp example/05_sql_direct.cpp \
// |         -o build/example_05
// |  Run:
// |    ./build/example_05                              # in-memory
// |    ./build/example_05 file:///tmp/atomdb.atoms   # persistent

#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/contracts/ICommandSource.hpp"
#include "atomdb/frontend/SqlParser.hpp"
#include "atomdb/storage/InMemoryStorageProvider.hpp"
#include "atomdb/storage/LocalFileStorageProvider.hpp"
#include "atomdb/types/Command.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Result.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/TxnId.hpp"
#include "atomdb/types/Value.hpp"

using namespace atomdb;

// Format a tuple as "col=val, col=val, ..."
static std::string fmtTuple(const Tuple& t) {
    std::ostringstream os;
    bool first = true;
    for (const auto& cv : t.columns()) {
        if (!first) os << ", ";
        first = false;
        os << cv.name << "=" << cv.value.toString();
    }
    return os.str();
}

int main(int argc, char** argv) {
    // Storage provider: in-memory by default, LocalFile if first argv is file:// URI.
    std::unique_ptr<IStorageProvider> storage;
    if (argc > 1 && std::string(argv[1]).rfind("file://", 0) == 0) {
        auto p = std::make_unique<LocalFileStorageProvider>();
        if (auto err = p->open(argv[1]); !err.isSentinel()) {
            std::cerr << "open: " << err.toString() << "\n";
            return 1;
        }
        storage = std::move(p);
        std::cout << "atomdb  persisting at " << argv[1] << "\n";
    } else {
        auto p = std::make_unique<InMemoryStorageProvider>();
        p->open("in-memory://");
        storage = std::move(p);
        std::cout << "atomdb  in-memory mode (no persistence)\n";
    }

    SqlParser parser;

    std::cout << "\n  Schema helpers:"
              << "\n    -- CREATE TABLE t (id INT PRIMARY KEY, name TEXT)"
              << "\n    -- For sharding: CREATE TABLE t (...) PARTITION BY HASH(id) PARTITIONS 4"
              << "\n    -- '.tables' lists registered tables"
              << "\n    -- 'EXIT' or Ctrl-D to quit"
              << "\n" << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        // Strip a trailing ';' if present.
        if (!line.empty() && line.back() == ';') line.pop_back();
        // Trim.
        std::size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        line = line.substr(first);
        std::size_t last = line.find_last_not_of(" \t\r\n");
        line = line.substr(0, last == std::string::npos ? line.size() : last + 1);
        if (line.empty()) continue;

        if (line == "EXIT" || line == ".quit" || line == ".exit") break;

        // Meta-commands.
        if (line == ".tables") {
            auto tbls = storage->tables();
            std::cout << "tables: ";
            for (std::size_t i = 0; i < tbls.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << tbls[i];
            }
            std::cout << "\n";
            continue;
        }
        if (line == ".help" || line == ".h") {
            std::cout << "SQL subset: CREATE TABLE, DROP TABLE,"
                         " INSERT, SELECT, UPDATE, DELETE.\n"
                         "Meta: .tables, .help, EXIT.\n";
            continue;
        }

        auto stmts = parser.parseAll(line);
        if (stmts.empty()) {
            std::cout << "[ERR] " << (parser.error().empty() ? "parse error" : parser.error()) << "\n";
            continue;
        }
        const auto& stmt = stmts[0];

        if (auto* ddl = std::get_if<DdlCreateTable>(&stmt)) {
            auto err = storage->createTable(ddl->schema);
            std::cout << (err.isSentinel() ? "[OK]" : "[ERR] " + std::string(err.toString())) << "\n";
            continue;
        }
        if (auto* ddl = std::get_if<DdlDropTable>(&stmt)) {
            auto err = storage->dropTable(ddl->table);
            std::cout << (err.isSentinel() ? "[OK]" : "[ERR] " + std::string(err.toString())) << "\n";
            continue;
        }
        if (auto* cmd = std::get_if<Command>(&stmt)) {
            // Minimal Command dispatcher. Engine is obtained via the provider's
            // engine() facet. This bypasses EngineLoop (no locks / txns).
            IStorageEngine& engine = *storage->engine();
            TransactionManager txnm;  // for visibleSeq on commit

            switch (cmd->type) {
                case CommandType::Select: {
                    ResultSet rs;
                    rs.success = true;
                    auto* engine_ptr = storage->engine();
                    engine_ptr->scan(TxnId{0}, cmd->table, [&](const Tuple& row) {
                        if (!cmd->where || cmd->where->evaluate(row)) {
                            if (cmd->projections.empty() ||
                                (cmd->projections.size() == 1 && cmd->projections[0] == "*")) {
                                rs.rows.push_back(row);
                            } else {
                                Tuple projected;
                                for (const auto& pc : cmd->projections) {
                                    if (pc == "*") continue;
                                    if (auto v = row.maybeGet(pc))
                                        projected.set(pc, std::move(*v));
                                }
                                rs.rows.push_back(std::move(projected));
                            }
                        }
                    });
                    if (rs.rows.empty()) {
                        std::cout << "[OK] (no rows)\n";
                        break;
                    }
                    std::cout << "rows: " << rs.rows.size() << "\n";
                    for (const auto& r : rs.rows)
                        std::cout << "  " << fmtTuple(r) << "\n";
                    break;
                }
                case CommandType::Insert: {
                    // For INSERT, we need a key. If the row has _id non-null, use it.
                    // Otherwise pass null() to let the engine auto-assign.
                    Value key = Value::null();
                    if (cmd->values) {
                        auto idOpt = cmd->values->maybeGet("_id");
                        if (idOpt && !idOpt->isNull()) key = *idOpt;
                    }
                    auto err = storage->engine()->put(TxnId{0}, cmd->table, key, *cmd->values);
                    if (!err.isSentinel()) {
                        std::cout << "[ERR] " << err.toString() << "\n";
                    } else {
                        std::cout << "[OK]\n";
                    }
                    break;
                }
                case CommandType::Update: {
                    if (!cmd->where) { std::cout << "[ERR] Update requires WHERE\n"; break; }
                    if (!cmd->values) { std::cout << "[ERR] Update requires values\n"; break; }

                    // Scan to find matching keys, then re-put each.
                    std::vector<Value> keysToUpdate;
                    storage->engine()->scan(TxnId{0}, cmd->table, [&](const Tuple& row) {
                        if (cmd->where->evaluate(row)) {
                            auto pkOpt = row.maybeGet("_id");
                            if (pkOpt) keysToUpdate.push_back(*pkOpt);
                        }
                    });
                    bool any = false;
                    for (const auto& k : keysToUpdate) {
                        Tuple row = *cmd->values;
                        row.set("_id", k);
                        auto err = storage->engine()->put(TxnId{0}, cmd->table, k, row);
                        if (!err.isSentinel()) { std::cout << "[ERR] " << err.toString() << "\n"; break; }
                        any = true;
                    }
                    if (!any) std::cout << "[ERR] no matching rows to update\n";
                    else std::cout << "[OK]\n";
                    break;
                }
                case CommandType::Delete: {
                    if (!cmd->where) { std::cout << "[ERR] Delete requires WHERE\n"; break; }
                    std::vector<Value> keysToDelete;
                    storage->engine()->scan(TxnId{0}, cmd->table, [&](const Tuple& row) {
                        if (cmd->where->evaluate(row)) {
                            auto pkOpt = row.maybeGet("_id");
                            if (pkOpt) keysToDelete.push_back(*pkOpt);
                        }
                    });
                    bool any = false;
                    for (const auto& k : keysToDelete) {
                        auto err = storage->engine()->remove(TxnId{0}, cmd->table, k);
                        if (!err.isSentinel()) { std::cout << "[ERR] " << err.toString() << "\n"; break; }
                        any = true;
                    }
                    if (!any) std::cout << "[ERR] no matching rows to delete\n";
                    else std::cout << "[OK]\n";
                    break;
                }
            }
            continue;
        }
        if (auto* txn = std::get_if<SqlTxnBegin>(&stmt)) {
            std::cout << "[OK] BEGIN (auto-commit mode; no-op)\n";
        } else if (std::get_if<SqlTxnCommit>(&stmt)) {
            std::cout << "[OK] COMMIT (auto-commit; no-op)\n";
        } else if (std::get_if<SqlTxnRollback>(&stmt)) {
            std::cout << "[OK] ROLLBACK (auto-commit; no-op)\n";
        }
    }

    storage->close();
    std::cout << "bye\n";
    return 0;
}