#ifndef ATOMDB_HTTP_API_HPP
#define ATOMDB_HTTP_API_HPP

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atomdb/contracts/IAccessPlugin.hpp"
#include "atomdb/contracts/IStorageProvider.hpp"
#include "atomdb/contracts/IStorageEngine.hpp"
#include "atomdb/core/EngineDispatcher.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/types/Command.hpp"
#include "atomdb/types/Result.hpp"
#include "atomdb/types/TxnId.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Value.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/Schema.hpp"
#include "atomdb/frontend/SqlParser.hpp"
#include "atomdb/frontend/JsonEncoder.hpp"
#include <iostream>

namespace atomdb {

// HttpApiAccessPlugin: minimal in-process HTTP/JSON front-end.
// Per spec §4.1/§4.2, this is an IAccessPlugin (Server mode) that speaks JSON.
// Since MSYS2 g++ 14 doesn't have C++23 <net> networking, this exposes a
// `handleRequest(reqJson)` method instead of binding a socket.
// A real deployment would wrap this in an ASIO/boost::beast/cpp-httplib server.
//
// Request format (compact JSON):
//   {"type":"query","sql":"SELECT * FROM users WHERE age > 30"}
//   {"type":"begin"}
//   {"type":"commit"}
//   {"type":"rollback"}
//   {"type":"query","sql":"INSERT INTO users VALUES (1, 'nikhil')","txnId":123}
//
// Response format:
//   {"success":true,"rows":[...]}
//   {"success":false,"error":"..."}
//   {"success":true,"txnId":123}
//   {"success":true} // for commit/rollback
class HttpApiAccessPlugin : public IAccessPlugin {
public:
    HttpApiAccessPlugin() : parser_(std::make_unique<SqlParser>()), opened_(false) {}

    // ---- IAccessPlugin ------------------------------------------------------
    std::string name() const override { return "http-api"; }
    AccessMode mode() const override { return AccessMode::Server; }
    std::uint32_t capabilities() const override {
        return static_cast<std::uint32_t>(AccessCapability::JsonWire)
             | static_cast<std::uint32_t>(AccessCapability::SqlWire)
             | static_cast<std::uint32_t>(AccessCapability::AsyncCapable);
    }

    DbError open(const std::string&, IStorageProvider* storage,
                 IEngineDispatcher* dispatcher) override {
        if (opened_) return DbError::internal("already open");
        if (!storage) return DbError::internal("storage provider required");
        if (!dispatcher) return DbError::internal("dispatcher required");
        storage_ = storage;
        dispatcher_ = dispatcher;
        opened_ = true;
        return DbError::sentinel();
    }

    DbError close() override {
        storage_ = nullptr;
        dispatcher_ = nullptr;
        opened_ = false;
        return DbError::sentinel();
    }

    bool isOpen() const noexcept override { return opened_; }

    void run() override {
        // Server mode: blocks on accept loop. In stub form, this does nothing;
        // the caller (test) invokes handleRequest() directly.
        while (opened_) {
            // no-op; in real impl this would accept connections
            // and enqueue HttpSession via dispatcher_->enqueue()
        }
    }

    std::string describeServer() const override {
        return "AtomDB HTTP/JSON API stub (no socket binding in v0.1)";
    }

    // ---- Test / direct invocation API --------------------------------------
    // Not part of IAccessPlugin; used by tests to simulate a request.
    std::string handleRequest(const std::string& requestJson) {
        if (!opened_) return JsonEncoder::encode(DbError::internal("plugin not open"));
        if (!dispatcher_) return JsonEncoder::encode(DbError::internal("no dispatcher"));

        // Parse request
        auto req = parseJsonRequest(requestJson);
        if (!req) return JsonEncoder::encode(DbError::parseError("invalid request JSON"));
        return executeRequest(*req);
    }

private:
    std::unique_ptr<SqlParser> parser_;
    IStorageProvider* storage_ = nullptr;
    IEngineDispatcher* dispatcher_ = nullptr;
    bool opened_ = false;

    // Local core components for direct execution
    TransactionManager txnm_;
    LockManager lockMgr_;
    DeadlockDetector deadlock_{lockMgr_};

    struct JsonRequest {
        std::string type;           // "query" | "begin" | "commit" | "rollback"
        std::string sql;            // for query
        std::optional<std::uint64_t> txnId; // optional
    };

    std::optional<JsonRequest> parseJsonRequest(const std::string& json) {
        JsonRequest req;
        auto findKey = [&](const std::string& key) -> std::optional<std::string> {
            std::string search = "\"" + key + "\"";
            auto p = json.find(search);
            if (p == std::string::npos) return std::nullopt;
            p = json.find(':', p);
            if (p == std::string::npos) return std::nullopt;
            p = json.find_first_not_of(" \t\n\r", p + 1);
            if (p == std::string::npos) return std::nullopt;
            if (json[p] == '"') {
                auto end = json.find('"', p + 1);
                if (end == std::string::npos) return std::nullopt;
                return json.substr(p + 1, end - p - 1);
            }
            auto end = json.find_first_of(",}", p);
            if (end == std::string::npos) return std::nullopt;
            return json.substr(p, end - p);
        };
        auto type = findKey("type");
        if (!type) return std::nullopt;
        req.type = *type;
        if (auto s = findKey("sql")) req.sql = *s;
        if (auto t = findKey("txnId")) {
            try { req.txnId = std::stoull(*t); } catch (...) {}
        }
        return req;
    }

    std::string executeRequest(const JsonRequest& req) {
        if (req.type == "begin") {
            TxnId tid = txnm_.beginTxn();
            return "{\"success\":true,\"txnId\":" + std::to_string(tid.value()) + "}";
        }
        if (req.type == "commit") {
            if (!req.txnId) return JsonEncoder::encode(DbError::notSupported("commit requires txnId"));
            TxnId txnId{*req.txnId};
            if (!txnm_.commitTxn(txnId)) {
                return JsonEncoder::encode(DbError::internal("txn not found or already closed"));
            }
            auto err = storage_->engine()->commit(txnId, txnm_.visibleSeq());
            if (!err.isSentinel()) return JsonEncoder::encode(err);
            return "{\"success\":true}";
        }
        if (req.type == "rollback") {
            if (!req.txnId) return JsonEncoder::encode(DbError::notSupported("rollback requires txnId"));
            TxnId txnId{*req.txnId};
            storage_->engine()->abort(txnId);
            txnm_.abortTxn(txnId);
            return "{\"success\":true}";
        }
        if (req.type == "query") {
            auto stmts = parser_->parseAll(req.sql);
            if (stmts.empty()) {
                return JsonEncoder::encode(DbError::parseError(parser_->error()));
            }
            return executeStatement(stmts[0], req.txnId);
        }
        return JsonEncoder::encode(DbError::notSupported("unknown request type: " + req.type));
    }

    std::string executeStatement(const SqlStatement& stmt, std::optional<std::uint64_t> explicitTxnId) {
        if (auto* ddl = std::get_if<DdlCreateTable>(&stmt)) {
            auto err = storage_->createTable(ddl->schema);
            if (!err.isSentinel()) return JsonEncoder::encode(err);
            return "{\"success\":true}";
        }
        if (auto* ddl = std::get_if<DdlDropTable>(&stmt)) {
            auto err = storage_->dropTable(ddl->table);
            if (!err.isSentinel()) return JsonEncoder::encode(err);
            return "{\"success\":true}";
        }
        if (std::get_if<SqlTxnBegin>(&stmt)) return executeRequest({"begin", "", {}});
        if (std::get_if<SqlTxnCommit>(&stmt)) return executeRequest({"commit", "", {}});
        if (std::get_if<SqlTxnRollback>(&stmt)) return executeRequest({"rollback", "", {}});

        if (auto* cmd = std::get_if<Command>(&stmt)) {
            return handleDml(*cmd, explicitTxnId);
        }
        return JsonEncoder::encode(DbError::internal("unhandled statement type"));
    }

    std::string handleDml(const Command& cmd, std::optional<std::uint64_t> explicitTxnId) {
        const bool auto_commit = !explicitTxnId.has_value();
        TxnId txnId = explicitTxnId ? TxnId{*explicitTxnId} : txnm_.beginTxn();

        LockMode mode = (cmd.type == CommandType::Select) ? LockMode::Shared : LockMode::Exclusive;

        // Deadlock pre-check
        auto cycle_victim = deadlock_.detectCycle(txnId);
        if (cycle_victim.has_value()) {
            txnm_.abortTxn(*cycle_victim);
            lockMgr_.release(*cycle_victim);
            return JsonEncoder::encode(DbError::deadlock("cycle detected for " + txnId.toString()));
        }

        // Acquire lock (blocks until granted)
        lockMgr_.acquire(txnId, cmd.table, mode);

        ResultSet rs;
        std::optional<DbError> error_opt;

        switch (cmd.type) {
            case CommandType::Select: {
                rs.success = true;
                storage_->engine()->scan(txnId, cmd.table, [&](const Tuple& row) {
                    if (!cmd.where || cmd.where->evaluate(row)) {
                        if (cmd.projections.empty() ||
                            (cmd.projections.size() == 1 && cmd.projections[0] == "*")) {
                            rs.rows.push_back(row);
                        } else {
                            Tuple projected;
                            for (const auto& pc : cmd.projections) {
                                if (pc == "*") continue;
                                auto v = row.maybeGet(pc);
                                if (v) projected.set(pc, std::move(*v));
                            }
                            rs.rows.push_back(std::move(projected));
                        }
                    }
                });
                break;
            }
            case CommandType::Insert: {
                Value key = Value::null();
                Tuple row;
                if (cmd.values) {
                    row = *cmd.values;
                    auto idOpt = row.maybeGet("_id");
                    if (idOpt && !idOpt->isNull()) key = *idOpt;
                }

                // ponytail: Phase 5 Item 7 — materialize column-level DEFAULTs
                // when INSERT INTO foo DEFAULT VALUES arrives with an empty
                // Tuple. For each declared column, fill it from the schema's
                // defaultValue if present; columns without a default remain
                // absent (the storage layer treats absent columns as NULL on
                // nullable / rejects on non-nullable).
                if (row.empty()) {
                    auto schema = storage_->describeTable(cmd.table);
                    if (schema) {
                        for (const auto& col : schema->columns) {
                            if (col.defaultValue.has_value()) {
                                row.set(col.name, *col.defaultValue);
                            }
                        }
                        // Re-extract _id from the materialized row so the
                        // auto-key path can still pick it up.
                        auto idOpt = row.maybeGet("_id");
                        if (idOpt && !idOpt->isNull()) key = *idOpt;
                    }
                }

                auto err = storage_->engine()->put(txnId, cmd.table, key, row);
                if (!err.isSentinel()) error_opt = err;
                else rs = ResultSet(true, {});
                break;
            }
            case CommandType::Update: {
                if (!cmd.where) { error_opt = DbError::internal("Update requires WHERE"); break; }
                if (!cmd.values) { error_opt = DbError::internal("Update requires values"); break; }
                std::vector<Value> keysToUpdate;
                storage_->engine()->scan(txnId, cmd.table, [&](const Tuple& row) {
                    if (cmd.where->evaluate(row)) {
                        auto pkOpt = row.maybeGet("_id");
                        if (pkOpt) keysToUpdate.push_back(*pkOpt);
                    }
                });
                bool any = false;
                for (const auto& k : keysToUpdate) {
                    Tuple row = *cmd.values;
                    row.set("_id", k);
                    auto err = storage_->engine()->put(txnId, cmd.table, k, row);
                    if (!err.isSentinel()) { error_opt = err; break; }
                    any = true;
                }
                if (!error_opt && !any) error_opt = DbError::notFound("no matching rows to update");
                else if (!error_opt) rs = ResultSet(true, {});
                break;
            }
            case CommandType::Delete: {
                if (!cmd.where) { error_opt = DbError::internal("Delete requires WHERE"); break; }
                std::vector<Value> keysToDelete;
                storage_->engine()->scan(txnId, cmd.table, [&](const Tuple& row) {
                    if (cmd.where->evaluate(row)) {
                        auto pkOpt = row.maybeGet("_id");
                        if (pkOpt) keysToDelete.push_back(*pkOpt);
                    }
                });
                bool any = false;
                for (const auto& k : keysToDelete) {
                    auto err = storage_->engine()->remove(txnId, cmd.table, k);
                    if (!err.isSentinel()) { error_opt = err; break; }
                    any = true;
                }
                if (!error_opt && !any) error_opt = DbError::notFound("no matching rows to delete");
                else if (!error_opt) rs = ResultSet(true, {});
                break;
            }
        }

        // Release the lock
        lockMgr_.release(txnId);

        if (error_opt) return JsonEncoder::encode(*error_opt);

        // Auto-commit if needed
        if (auto_commit) {
            txnm_.commitTxn(txnId);
            auto commitErr = storage_->engine()->commit(txnId, txnm_.visibleSeq());
            if (!commitErr.isSentinel()) return JsonEncoder::encode(commitErr);
        }
        return JsonEncoder::encode(rs);
    }
};

} // namespace atomdb

#endif // ATOMDB_HTTP_API_HPP