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
            case ValueType::Blob:    return "\"" + base64Encode(v.asBlob()) + "\"";
            case ValueType::Date:    return "\"" + encodeIsoDate(v.asDate()) + "\"";
            case ValueType::Timestamp: return "\"" + encodeIsoTimestamp(v.asTimestamp()) + "\"";
        }
        return "null";
    }

    // Optional compact wire format: array-of-arrays instead of
    // object-of-columns. Saves bytes for big result sets when the
    // caller already knows the schema.
    static std::string encodeArray(const Tuple& t) {
        std::ostringstream os;
        os << "[";
        const auto& cols = t.columns();
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (i) os << ',';
            os << encode(cols[i].value);
        }
        os << "]";
        return os.str();
    }

    static std::string encodeArray(const ResultSet& rs) {
        std::ostringstream os;
        os << "{\"success\":" << (rs.success ? "true" : "false") << ",\"rows\":[";
        for (std::size_t i = 0; i < rs.rows.size(); ++i) {
            if (i) os << ',';
            os << encodeArray(rs.rows[i]);
        }
        os << "]}";
        return os.str();
    }

    // ponytail: standard Base64 encoder so Blob round-trips through JSON
    // faithfully. RFC 4648 alphabet.
    static std::string base64Encode(const std::vector<std::uint8_t>& bytes) {
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((bytes.size() + 2) / 3) * 4);
        std::size_t i = 0;
        while (i + 3 <= bytes.size()) {
            std::uint32_t n = (std::uint32_t(bytes[i]) << 16) |
                              (std::uint32_t(bytes[i+1]) << 8) |
                              std::uint32_t(bytes[i+2]);
            out.push_back(kAlphabet[(n >> 18) & 0x3F]);
            out.push_back(kAlphabet[(n >> 12) & 0x3F]);
            out.push_back(kAlphabet[(n >> 6) & 0x3F]);
            out.push_back(kAlphabet[n & 0x3F]);
            i += 3;
        }
        std::size_t rem = bytes.size() - i;
        if (rem == 1) {
            std::uint32_t n = std::uint32_t(bytes[i]) << 16;
            out.push_back(kAlphabet[(n >> 18) & 0x3F]);
            out.push_back(kAlphabet[(n >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        } else if (rem == 2) {
            std::uint32_t n = (std::uint32_t(bytes[i]) << 16) |
                              (std::uint32_t(bytes[i+1]) << 8);
            out.push_back(kAlphabet[(n >> 18) & 0x3F]);
            out.push_back(kAlphabet[(n >> 12) & 0x3F]);
            out.push_back(kAlphabet[(n >> 6) & 0x3F]);
            out.push_back('=');
        }
        return out;
    }

    // ponytail: ISO-8601 date formatter (YYYY-MM-DD) from int32 days since
    // 1970-01-01, proleptic Gregorian.
    static std::string encodeIsoDate(std::int32_t days) {
        std::int64_t z = std::int64_t(days) + 719468;
        std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        std::int64_t doe = z - era * 146097;
        std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        std::int64_t y = yoe + era * 400;
        std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        std::int64_t mp = (5 * doy + 2) / 153;
        std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
        std::int64_t m = mp + (mp < 10 ? 3 : -9);
        y += (m <= 2 ? 1 : 0);
        std::ostringstream os;
        os << std::setfill('0') << std::setw(4) << y << '-'
           << std::setw(2) << m << '-' << std::setw(2) << d
           << std::setfill(' ');
        return os.str();
    }

    // ISO-8601 timestamp (YYYY-MM-DDThh:mm:ss.fffZ) from int64 ms since epoch.
    static std::string encodeIsoTimestamp(std::int64_t ms) {
        std::int32_t days = std::int32_t(ms / 86400000);
        std::int64_t rem = ms % 86400000;
        if (rem < 0) { rem += 86400000; days -= 1; }
        std::string d = encodeIsoDate(days);
        std::int32_t h = std::int32_t(rem / 3600000);
        rem %= 3600000;
        std::int32_t mn = std::int32_t(rem / 60000);
        rem %= 60000;
        std::int32_t s = std::int32_t(rem / 1000);
        std::int32_t fff = std::int32_t(rem % 1000);
        std::ostringstream os;
        os << d << 'T'
           << std::setfill('0') << std::setw(2) << h << ':'
           << std::setw(2) << mn << ':' << std::setw(2) << s
           << '.' << std::setw(3) << fff << 'Z'
           << std::setfill(' ');
        return os.str();
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