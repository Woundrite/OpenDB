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
        // Parse the JSON request and return a Command or special txn command
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
    struct JsonRequest {
        std::string type;           // "query" | "begin" | "commit" | "rollback"
        std::string sql;            // for query
        std::optional<std::uint64_t> txnId; // optional
    };

    // Parse the simple JSON request
std::optional<JsonRequest> parseJsonRequest(const std::string& requestJson) {
            JsonRequest req;
            auto findKey = [&](const std::string& key) -> std::optional<std::string> {
                std::string search = "\"" + key + "\"";
                auto p = requestJson.find(search);
                if (p == std::string::npos) return std::nullopt;
                p = requestJson.find(':', p);
                if (p == std::string::npos) return std::nullopt;
                p = requestJson.find_first_not_of(" \t\n\r", p + 1);
                if (p == std::string::npos) return std::nullopt;
                if (requestJson[p] == '"') {
                    auto end = requestJson.find('"', p + 1);
                    if (end == std::string::npos) return std::nullopt;
                    return requestJson.substr(p + 1, end - p - 1);
                }
                auto end = requestJson.find_first_of(",}", p);
                if (end == std::string::npos) return std::nullopt;
                return requestJson.substr(p, end - p);
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