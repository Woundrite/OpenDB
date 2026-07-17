#ifndef ATOMDB_HTTP_API_HPP
#define ATOMDB_HTTP_API_HPP

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atomdb/contracts/IAccessPlugin.hpp"
#include "atomdb/contracts/ICommandSource.hpp"
#include "atomdb/contracts/IStorageProvider.hpp"
#include "atomdb/contracts/IStorageEngine.hpp"
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
// Since MSYS2 g++ 14 doesn't have C++23 <net> networking, this is a stub that
// exposes a `handleRequest(reqJson)` method instead of binding a socket.
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
        auto req = parseJsonRequest(requestJson);
        if (!req) return JsonEncoder::encode(DbError::parseError("invalid request JSON"));
        return handleParsedRequest(*req);
    }

private:
    std::unique_ptr<SqlParser> parser_;
    IStorageProvider* storage_ = nullptr;
    IEngineDispatcher* dispatcher_ = nullptr;
    bool opened_ = false;

    // Parse and dispatch a parsed JsonRequest
    std::string handleParsedRequest(const JsonRequest& req) {
        if (req.type == "begin") {
            return handleBegin();
        }
        if (req.type == "commit") {
            return handleCommit(req.txnId);
        }
        if (req.type == "rollback") {
            return handleRollback(req.txnId);
        }
        if (req.type == "query") {
            return handleQuery(req.sql, req.txnId);
        }
        return JsonEncoder::encode(DbError::notSupported("unknown request type: " + req.type));
    }

    std::string handleBegin() {
        TxnId tid = txnm_.beginTxn();
        return "{\"success\":true,\"txnId\":" + std::to_string(tid.value()) + "}";
    }

    std::string handleCommit(std::optional<std::uint64_t> txnIdOpt) {
        if (!txnIdOpt) {
            return JsonEncoder::encode(DbError::notSupported("commit requires txnId"));
        }
        TxnId txnId{*txnIdOpt};
        if (!txnm_.commitTxn(txnId)) {
            return JsonEncoder::encode(DbError::internal("txn not found or already closed"));
        }
        auto err = storage_->engine()->commit(txnId, txnm_.visibleSeq());
        if (!err.isSentinel()) return JsonEncoder::encode(err);
        return "{\"success\":true}";
    }

    std::string handleRollback(std::optional<std::uint64_t> txnIdOpt) {
        if (!txnIdOpt) {
            return JsonEncoder::encode(DbError::notSupported("rollback requires txnId"));
        }
        TxnId txnId{*txnIdOpt};
        storage_->engine()->abort(txnId);
        txnm_.abortTxn(txnId);
        return "{\"success\":true}";
    }

    std::string handleQuery(const std::string& sql, std::optional<std::uint64_t> explicitTxnId) {
        // Parse SQL into SqlStatements
        auto stmts = parser_->parseAll(sql);
        if (stmts.empty()) {
            return JsonEncoder::encode(DbError::parseError(parser_->error()));
        }

        // For simplicity, execute first statement only (single-statement API)
        // In real impl, we'd iterate and commit between statements.
        return executeStatement(stmts[0], explicitTxnId);
    }

    std::string executeStatement(const SqlStatement& stmt, std::optional<std::uint64_t> explicitTxnId) {
        // DDL: CREATE/DROP TABLE
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
        if (std::get_if<SqlTxnBegin>(&stmt)) return handleBegin();
        if (std::get_if<SqlTxnCommit>(&stmt)) return handleCommit(explicitTxnId);
        if (std::get_if<SqlTxnRollback>(&stmt)) return handleRollback(explicitTxnId);

        // DML: Command
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
            lock_mgr_.release(*cycle_victim);
            return JsonEncoder::encode(DbError::deadlock("cycle detected for " + txnId.toString()));
        }

        // Acquire lock (blocks until granted)
        lock_mgr_.acquire(txnId, cmd.table, mode);

        // Dispatch by command type
        ResultSet rs;
        std::optional<DbError> error_opt;

        switch (cmd.type) {
            case CommandType::Select: {
                rs.success = true;
                storage_->engine()->scan(txnId, cmd.table, [&](const Tuple& row) {
                    if (!cmd.where.has_value() || cmd.where->evaluate(row)) {
                        // "*" projection means all columns, treat as no projection
                        bool wantsAll = cmd.projections.empty() ||
                            (cmd.projections.size() == 1 && cmd.projections[0] == "*");
                        if (wantsAll) {
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
                auto idOpt = cmd.values->maybeGet("_id");
                if (idOpt && !idOpt->isNull()) key = *idOpt;
                auto err = storage_->engine()->put(txnId, cmd.table, key, *cmd.values);
                if (!err.isSentinel()) error_opt = err;
                else rs = ResultSet(true, {}); // success
                break;
            }
            case CommandType::Update: {
                if (!cmd.where) { error_opt = DbError::internal("Update requires WHERE"); break; }
                if (!cmd.values) { error_opt = DbError::internal("Update requires values"); break; }
                // Scan to find matching keys, then re-put each
                std::vector<Value> keysToUpdate;
                storage_->engine()->scan(txnId, cmd.table, [&](const Tuple& row) {
                    if (cmd.where->evaluate(row)) {
                        auto pkOpt = row.maybeGet(cmd.table.empty() ? "_id" : cmd.table);
                        if (pkOpt) keysToUpdate.push_back(*pkOpt);
                    }
                });
                bool any = false;
                for (const auto& k : keysToUpdate) {
                    Tuple row = *cmd.values;
                    // Ensure pk is preserved
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
        lock_mgr_.release(txnId);

        if (error_opt) return JsonEncoder::encode(*error_opt);

        // Auto-commit if needed
        if (auto_commit) {
            txnm_.commitTxn(txnId);
            auto commitErr = storage_->engine()->commit(txnId, txnm_.visibleSeq());
            if (!commitErr.isSentinel()) return JsonEncoder::encode(commitErr);
        }
        return JsonEncoder::encode(rs);
    }

    // State
    TransactionManager txnm_;
    LockManager lock_mgr_;
    DeadlockDetector deadlock_{lock_mgr_};
};

} // namespace atomdb

#endif // ATOMDB_HTTP_API_HPP