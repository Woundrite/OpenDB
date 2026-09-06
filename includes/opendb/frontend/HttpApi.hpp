#ifndef OPENDB_HTTP_API_HPP
#define OPENDB_HTTP_API_HPP

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "opendb/contracts/IAccessPlugin.hpp"
#include "opendb/contracts/IStorageProvider.hpp"
#include "opendb/contracts/IStorageEngine.hpp"
#include "opendb/core/EngineDispatcher.hpp"
#include "opendb/core/TransactionManager.hpp"
#include "opendb/core/LockManager.hpp"
#include "opendb/core/DeadlockDetector.hpp"
#include "opendb/types/Command.hpp"
#include "opendb/types/Result.hpp"
#include "opendb/types/TxnId.hpp"
#include "opendb/types/DbError.hpp"
#include "opendb/types/Value.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/Schema.hpp"
#include "opendb/frontend/SqlParser.hpp"
#include "opendb/frontend/JsonEncoder.hpp"
#include <iostream>

namespace opendb {

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
    // Constructor accepting shared core components
    HttpApiAccessPlugin(TransactionManager& txnm,
                         LockManager& lkm,
                         DeadlockDetector& dd,
                         std::unique_ptr<SqlParser> parser = std::make_unique<SqlParser>())
        : parser_(std::move(parser)), opened_(false), txnm_(txnm), lockMgr_(lkm), deadlock_(dd) {}

    // ---- IAccessPlugin ------------------------------------------------------
    std::string name() const override { return "http-api"; }
    std::string describeServer() const override {
        return "OpenDB HTTP/JSON API stub (no socket binding in v0.1)";
    }
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

    // ---- Test / direct invocation API --------------------------------------
    // Not part of IAccessPlugin; used by tests to simulate a request.
    std::string handleRequest(const std::string& requestJson) {
        if (!opened_) return JsonEncoder::encode(DbError::internal("plugin not open"));
        if (!dispatcher_) return JsonEncoder::encode(DbError::internal("no dispatcher"));

        // Parse request using the new proper JSON parser
        auto req = parseJsonRequest(requestJson);
        if (!req) return JsonEncoder::encode(DbError::parseError("invalid request JSON"));
        return executeRequest(*req);
    }

private:
    std::unique_ptr<SqlParser> parser_;
    IStorageProvider* storage_ = nullptr;
    IEngineDispatcher* dispatcher_ = nullptr;
    bool opened_ = false;

    // Shared core components (non-owning references)
    TransactionManager& txnm_;
    LockManager& lockMgr_;
    DeadlockDetector& deadlock_;

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
            // Release the lock held for this transaction
            lockMgr_.release(txnId);
            return "{\"success\":true}";
        }
        if (req.type == "rollback") {
            if (!req.txnId) return JsonEncoder::encode(DbError::notSupported("rollback requires txnId"));
            TxnId txnId{*req.txnId};
            storage_->engine()->abort(txnId);
            txnm_.abortTxn(txnId);
            // Release the lock held for this transaction
            lockMgr_.release(txnId);
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
        if (auto* ddl = std::get_if<DdlAlterTable>(&stmt)) {
            auto err = storage_->alterTable(ddl->table, ddl->spec);
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

        // Acquire lock with timeout (default 50s per spec C.5)
        using opendb::LockAcquireResult;
        auto acquireResult = lockMgr_.tryAcquire(txnId, cmd.table, mode, std::chrono::seconds(50));
        if (acquireResult == LockAcquireResult::TimedOut) {
            txnm_.abortTxn(txnId);
            lockMgr_.release(txnId);
            return JsonEncoder::encode(DbError::lockTimeout("lock wait exceeded 50000 ms"));
        }

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

        // Only release lock and auto-commit if NOT in an explicit transaction
        if (auto_commit) {
            lockMgr_.release(txnId);

            if (error_opt) return JsonEncoder::encode(*error_opt);

            // Auto-commit
            txnm_.commitTxn(txnId);
            auto commitErr = storage_->engine()->commit(txnId, txnm_.visibleSeq());
            if (!commitErr.isSentinel()) return JsonEncoder::encode(commitErr);
        } else {
            // In explicit transaction: don't release lock, don't auto-commit
            // Lock will be released on COMMIT/ROLLBACK
            if (error_opt) return JsonEncoder::encode(*error_opt);
        }
        return JsonEncoder::encode(rs);
    }
};

} // namespace opendb

#endif // OPENDB_HTTP_API_HPP