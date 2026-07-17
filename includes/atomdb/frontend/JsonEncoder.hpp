#ifndef ATOMDB_JSON_ENCODER_HPP
#define ATOMDB_JSON_ENCODER_HPP

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "atomdb/types/Result.hpp"
#include "atomdb/types/Value.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/DbError.hpp"

namespace atomdb {

// JsonEncoder: minimal JSON serializer for test/stub purposes.
// Not a full JSON library; only encodes the subset AtomDB needs for
// HTTP API test fixtures. No unescaping, no UTF-8 validation.
class JsonEncoder {
public:
    static std::string encode(const ResultSet& rs) {
        std::ostringstream os;
        os << "{\"success\":" << (rs.success ? "true" : "false") << ",\"rows\":[";
        for (std::size_t i = 0; i < rs.rows.size(); ++i) {
            if (i) os << ',';
            os << encodeTuple(rs.rows[i]);
        }
        os << "]}";
        return os.str();
    }

    static std::string encode(const DbError& err) {
        std::ostringstream os;
        os << "{\"success\":false,\"error\":\"" << escapeJson(err.toString()) << "\"}";
        return os.str();
    }

    static std::string encode(const Value& v) {
        switch (v.type()) {
            case ValueType::Null:    return "null";
            case ValueType::Bool:    return v.asBool() ? "true" : "false";
            case ValueType::Int32:   return std::to_string(v.asInt32());
            case ValueType::Int64:   return std::to_string(v.asInt64());
            case ValueType::Double:  return std::to_string(v.asDouble());
            case ValueType::Text:    return "\"" + escapeJson(v.asText()) + "\"";
            case ValueType::Blob:    return "\"[BLOB " + std::to_string(v.asBlob().size()) + " bytes]\"";
            case ValueType::Date:    return std::to_string(v.asDate());
            case ValueType::Timestamp: return std::to_string(v.asTimestamp());
        }
        return "null";
    }

private:
    static std::string encodeTuple(const Tuple& t) {
        std::ostringstream os;
        os << "{";
        const auto& cols = t.columns();
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (i) os << ',';
            os << "\"" << escapeJson(cols[i].name) << "\":" << encode(cols[i].value);
        }
        os << "}";
        return os.str();
    }

    static std::string escapeJson(const std::string& s) {
        std::ostringstream os;
        for (unsigned char c : s) {
            switch (c) {
                case '"':  os << "\\\""; break;
                case '\\': os << "\\\\"; break;
                case '\b': os << "\\b";  break;
                case '\f': os << "\\f";  break;
                case '\n': os << "\\n";  break;
                case '\r': os << "\\r";  break;
                case '\t': os << "\\t";  break;
                default:
                    if (c < 0x20) {
                        os << "\\u" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                        os << std::dec << std::nouppercase << std::setfill(' ');
                    } else {
                        os << c;
                    }
            }
        }
        return os.str();
    }
};

} // namespace atomdb

// Minimal JSON request parser for HTTP API test fixtures.
// Only handles our request shapes: {"type":"query","sql":"...","txnId":123}
// {"type":"begin"} etc.
struct JsonRequest {
    std::string type;           // "query" | "begin" | "commit" | "rollback"
    std::string sql;            // for query
    std::optional<std::uint64_t> txnId; // optional for explicit txn
};

inline std::optional<JsonRequest> parseJsonRequest(const std::string& json) {
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

#endif // ATOMDB_JSON_ENCODER_HPP