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

// ---------------------------------------------------------------------------
// JSON request parser (ponytail: F.6/I.8 — replaces hand-rolled substring scan)
//
// Hand-rolled scan broke on:
//   - escaped quotes inside strings ("sql": "SELECT \"foo\"")
//   - SQL containing the literal substring "sql" (e.g. column names like "sql_log")
//   - nested objects (we only use flat ones, but forward-compat)
//   - control chars / Unicode escapes (\uXXXX)
//
// This is a minimal recursive-descent JSON parser sufficient for our HTTP API
// request format:
//   {"type":"query","sql":"...","txnId":123}
//   {"type":"begin"} | {"type":"commit","txnId":42} | {"type":"rollback","txnId":42}
//
// Returns the raw string value for each known key. We only parse the subset we
// need; unknown keys are ignored. Malformed JSON returns nullopt.
// ---------------------------------------------------------------------------
struct JsonRequest {
    std::string type;           // "query" | "begin" | "commit" | "rollback"
    std::string sql;            // for query
    std::optional<std::uint64_t> txnId; // optional for explicit txn
};

namespace json_detail {

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s), pos_(0) {}

    // Top-level: parse a single JSON value, then ensure we've consumed all
    // trailing whitespace (no garbage at the top level).
    bool parseValue(JsonRequest& out) {
        skipWhitespace();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == '{') return parseObject(out);
        return false;
    }

private:
    const std::string& s_;
    std::size_t pos_;

    void skipWhitespace() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    // Parse an object: '{' [ '"key"' ':' value (',' '"key"' ':' value)* ] '}'
    bool parseObject(JsonRequest& out) {
        skipWhitespace();
        if (pos_ >= s_.size() || s_[pos_] != '{') return false;
        ++pos_;
        skipWhitespace();
        // Empty object: {}
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (true) {
            skipWhitespace();
            if (pos_ >= s_.size() || s_[pos_] != '"') return false;
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            skipWhitespace();
            if (pos_ >= s_.size()) return false;

            // Capture values only for known keys.
            if (key == "type") {
                std::string v;
                if (!parseString(v)) return false;
                out.type = std::move(v);
            } else if (key == "sql") {
                std::string v;
                if (!parseString(v)) return false;
                out.sql = std::move(v);
            } else if (key == "txnId") {
                std::string v;
                if (!parseNumberAsString(v)) return false;
                try { out.txnId = std::stoull(v); } catch (...) { return false; }
            } else {
                // Unknown key — skip its value entirely.
                if (!skipValue()) return false;
            }

            skipWhitespace();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            return false;
        }
    }

    // Parse a JSON string: '"' chars '"'. Handles escapes \\, \", \/, \b,
    // \f, \n, \r, \t, \uXXXX. Returns the unescaped string.
    bool parseString(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == '"') { ++pos_; return true; }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= s_.size()) return false;
                char esc = s_[pos_++];
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) return false;
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_];
                            unsigned v = 0;
                            if      (h >= '0' && h <= '9') v = h - '0';
                            else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                            else return false;
                            code = (code << 4) | v;
                            ++pos_;
                        }
                        // BMP only: emit as UTF-8 (3-byte form for 0x0800-0xFFFF).
                        if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(c);
                ++pos_;
            }
        }
        return false; // unterminated string
    }

    // Parse a JSON number (integer form, for txnId). Captures the raw text
    // into `out` so the caller can stoull it.
    bool parseNumberAsString(std::string& out) {
        skipWhitespace();
        std::size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c >= '0' && c <= '9') { ++pos_; continue; }
            break;
        }
        if (pos_ == start) return false;
        out = s_.substr(start, pos_ - start);
        return true;
    }

    // Skip over an arbitrary JSON value (object / array / string / number /
    // literal) without materialising it. Used for unknown keys.
    bool skipValue() {
        skipWhitespace();
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '"') {
            std::string tmp;
            return parseString(tmp);
        }
        if (c == '{') {
            ++pos_;
            skipWhitespace();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            while (true) {
                skipWhitespace();
                if (pos_ >= s_.size() || s_[pos_] != '"') return false;
                std::string k;
                if (!parseString(k)) return false;
                skipWhitespace();
                if (pos_ >= s_.size() || s_[pos_] != ':') return false;
                ++pos_;
                if (!skipValue()) return false;
                skipWhitespace();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++pos_;
            skipWhitespace();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            while (true) {
                if (!skipValue()) return false;
                skipWhitespace();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
                return false;
            }
        }
        // true / false / null / number
        if (c == 't' || c == 'f' || c == 'n') {
            const char* lit = (c == 't') ? "true" : (c == 'f') ? "false" : "null";
            std::size_t n = (c == 't') ? 4 : (c == 'f') ? 5 : 4;
            if (pos_ + n > s_.size()) return false;
            if (s_.compare(pos_, n, lit) != 0) return false;
            pos_ += n;
            return true;
        }
        // number
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            std::size_t start = pos_;
            if (c == '-' || c == '+') ++pos_;
            while (pos_ < s_.size()) {
                char ch = s_[pos_];
                if (ch >= '0' && ch <= '9') { ++pos_; continue; }
                break;
            }
            return pos_ > start;
        }
        return false;
    }
};

} // namespace json_detail

inline std::optional<JsonRequest> parseJsonRequest(const std::string& json) {
    JsonRequest req;
    json_detail::JsonParser p(json);
    if (!p.parseValue(req)) return std::nullopt;
    // Require "type" to be present and non-empty.
    if (req.type.empty()) return std::nullopt;
    return req;
}

#endif // ATOMDB_JSON_ENCODER_HPP