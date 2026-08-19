#ifndef ATOMDB_HTTP_SESSION_HPP
#define ATOMDB_HTTP_SESSION_HPP

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atomdb/contracts/IAccessPlugin.hpp"
#include "atomdb/contracts/ICommandSource.hpp"
#include "atomdb/contracts/IStorageProvider.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/types/Command.hpp"
#include "atomdb/types/Result.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/TxnId.hpp"
#include "atomdb/types/Value.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/Schema.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/frontend/SqlParser.hpp"
#include "atomdb/frontend/JsonEncoder.hpp"

namespace atomdb {

// HttpSession: an ISession that processes a single HTTP request/response cycle.
// When enqueued to EngineDispatcher, the worker thread calls run() which:
//  1. Parses the HTTP request (simulated via JSON string)
//  2. Executes the SQL via storage engine
//  3. Returns JSON response via ICommandSource::present()
class HttpSession : public ISession {
public:
    struct RequestContext {
        std::string requestJson;
        IStorageProvider* storage;
        SqlParser* parser;
        TransactionManager& txnm;
        LockManager& lockMgr;
        DeadlockDetector& deadlock;
    };

    explicit HttpSession(RequestContext ctx)
        : ctx_(std::move(ctx)), requestProcessed_(false) {}

    ~HttpSession() override = default;

    // ICommandSource
    std::optional<Command> nextCommand() override {
        if (requestProcessed_) return std::nullopt; // EOF after one request
        requestProcessed_ = true;
        // Parse the JSON request using the new proper JSON parser
        auto req = parseJsonRequest(ctx_.requestJson);
        if (!req) {
            // Parse error - will handle in run() by presenting error
            return std::nullopt;
        }
        return convertRequestToCommand(*req);
    }

    void present(const ResultSet& rs) override {
        responseJson_ = JsonEncoder::encode(rs);
    }

    void present(const DbError& err) override {
        responseJson_ = JsonEncoder::encode(err);
    }

    // ISession
    void close() override {
        closed_ = true;
    }

    // For testing: get the response JSON
    const std::string& getResponse() const noexcept { return responseJson_; }
    bool isClosed() const noexcept { return closed_; }

private:
    // Convert JsonRequest to Command or special txn control
    std::optional<Command> convertRequestToCommand(const JsonRequest& req) {
        if (req.type == "begin") return Command(CommandType::Insert, "__begin__", std::nullopt, std::nullopt, {}, std::nullopt);
        if (req.type == "commit") return Command(CommandType::Insert, "__commit__", std::nullopt, std::nullopt, {}, std::nullopt);
        if (req.type == "rollback") return Command(CommandType::Insert, "__rollback__", std::nullopt, std::nullopt, {}, std::nullopt);
        if (req.type == "query") {
            auto stmts = ctx_.parser->parseAll(req.sql);
            if (stmts.empty()) return std::nullopt;
            const auto& stmt = stmts[0];
            if (auto* cmd = std::get_if<Command>(&stmt)) {
                return *cmd;
            }
        }
        return std::nullopt;
    }

    RequestContext ctx_;
    bool requestProcessed_ = false;
    std::string responseJson_;
    bool closed_ = false;
};

} // namespace atomdb

#endif // ATOMDB_HTTP_SESSION_HPP