#ifndef OPENDB_VALUE_HPP
#define OPENDB_VALUE_HPP

#include <cstdint>
#include <cstring>
#include <ostream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// Value: the tagged-union scalar of OpenDB (spec §3.2).
//
// Nine tags (extended in Phase 4 for pluggable-provider type vocabulary):
//   Null, Bool, Int32, Int64, Double, Text,       -- v0.1 (6 tags)
//   Blob, Date, Timestamp                          -- v0.2 (3 tags for providers)
//
// Storage layout:
//   - Blob      is stored as std::vector<std::uint8_t>.
//   - Date      is stored as std::int32_t (days since 1970-01-01, proleptic Gregorian).
//   - Timestamp is stored as std::int64_t (milliseconds since 1970-01-01 UTC).
//   Date reuses the Int32 slot in the variant; Timestamp reuses the Int64 slot;
//   they are distinguished by tag_, never by variant index alone.
//
// Notes:
//   - Int32 and Int64 are kept distinct in the variant for type fidelity, but
//     `compare()` orders them numerically as a combined Number category.
//     Therefore Int32(5).compare(Int64(5)) == std::strong_ordering::equal.
//   - Date and Timestamp are also in the Number bucket for `compare()`, so
//     a Date and Int64 with the same numeric value compare equal — but tag-true
//     operator== returns false (Date(0) != Int32(0)). This mirrors the existing
//     Int32/Int64 behavior and lets SQL comparison work seamlessly across
//     numeric types.
//   - `operator==` is tag-true equality: two Values are equal iff they share
//     the exact same tag and the same payload.
//
// Ordering categories:
//   Null  <  Bool  <  Number(Int32|Int64|Double|Date|Timestamp)  <  Text  <  Blob
// Within Bool: false < true.
// Within Number: numeric magnitude (negative < positive; same magnitude equal).
// Within Text: std::string lexicographic byte comparison.
// Within Blob: lexicographic byte comparison on the raw vector (like Text).

namespace opendb {

enum class ValueType {
    Null,
    Bool,
    Int32,
    Int64,
    Double,
    Text,
    // --- Phase 4 extensions (provider-declared; core never asserts they're supported) ---
    Blob,       // std::vector<std::uint8_t>
    Date,       // std::int32_t days since 1970-01-01
    Timestamp,  // std::int64_t milliseconds since 1970-01-01 UTC
};

class Value {
public:
    // ---- named constructors ---------------------------------------------------
    static Value null()      { Value v; v.tag_ = ValueType::Null; return v; }
    static Value boolean(bool x) {
        Value v; v.tag_ = ValueType::Bool; v.storage_ = x; return v;
    }
    static Value int32(std::int32_t x) {
        Value v; v.tag_ = ValueType::Int32; v.storage_ = x; return v;
    }
    static Value int64(std::int64_t x) {
        Value v; v.tag_ = ValueType::Int64; v.storage_ = x; return v;
    }
    // Convenience: 'integer' picks Int64 by default, the wider type.
    static Value integer(std::int64_t x) {
        return int64(x);
    }
    static Value real(double x) {
        Value v; v.tag_ = ValueType::Double; v.storage_ = x; return v;
    }
    static Value text(std::string x) {
        Value v; v.tag_ = ValueType::Text; v.storage_ = std::move(x); return v;
    }
    // Blob: unstructured bytes. (de)serialized via the binary wire format below.
    static Value blob(std::vector<std::uint8_t> x) {
        Value v; v.tag_ = ValueType::Blob; v.storage_ = std::move(x); return v;
    }
    // Date: days since 1970-01-01 (proleptic Gregorian). Allows negatives.
    static Value date(std::int32_t days) {
        Value v; v.tag_ = ValueType::Date; v.storage_ = days; return v;
    }
    // Timestamp: milliseconds since 1970-01-01 UTC.
    static Value timestamp(std::int64_t millis) {
        Value v; v.tag_ = ValueType::Timestamp; v.storage_ = millis; return v;
    }

    // ---- observations ---------------------------------------------------------
    ValueType type() const noexcept { return tag_; }

    bool isNull()      const noexcept { return tag_ == ValueType::Null; }
    bool isBool()      const noexcept { return tag_ == ValueType::Bool; }
    bool isInt32()     const noexcept { return tag_ == ValueType::Int32; }
    bool isInt64()     const noexcept { return tag_ == ValueType::Int64; }
    bool isDouble()    const noexcept { return tag_ == ValueType::Double; }
    bool isText()      const noexcept { return tag_ == ValueType::Text; }
    bool isBlob()      const noexcept { return tag_ == ValueType::Blob; }
    bool isDate()      const noexcept { return tag_ == ValueType::Date; }
    bool isTimestamp() const noexcept { return tag_ == ValueType::Timestamp; }
    bool isNumber()    const noexcept {
        return tag_ == ValueType::Int32 ||
               tag_ == ValueType::Int64 ||
               tag_ == ValueType::Double ||
               tag_ == ValueType::Date ||
               tag_ == ValueType::Timestamp;
    }

    // UB if type mismatch (documented; no throw for hot-path speed).
    bool          asBool()      const { return std::get<bool>(storage_); }
    std::int32_t  asInt32()     const { return std::get<std::int32_t>(storage_); }
    std::int64_t  asInt64()     const { return std::get<std::int64_t>(storage_); }
    double        asDouble()    const { return std::get<double>(storage_); }
    const std::string& asText() const { return std::get<std::string>(storage_); }
    // Date reuses the Int32 slot; Timestamp reuses the Int64 slot.
    std::int32_t  asDate()      const { return std::get<std::int32_t>(storage_); }
    std::int64_t  asTimestamp() const { return std::get<std::int64_t>(storage_); }
    const std::vector<std::uint8_t>& asBlob() const {
        return std::get<std::vector<std::uint8_t>>(storage_);
    }

    // ---- ordering -------------------------------------------------------------
    // Returns a strong_ordering driving all relational comparisons.
    // Buckets: Null(0) < Bool(1) < Number(2) < Text(3) < Blob(4)
    std::strong_ordering compare(const Value& other) const {
        const auto bucket = [](ValueType t) -> int {
            switch (t) {
                case ValueType::Null:      return 0;
                case ValueType::Bool:       return 1;
                case ValueType::Int32:
                case ValueType::Int64:
                case ValueType::Double:
                case ValueType::Date:
                case ValueType::Timestamp: return 2; // shared Number bucket
                case ValueType::Text:      return 3;
                case ValueType::Blob:      return 4;
            }
            return 5;
        };
        const int a = bucket(tag_);
        const int b = bucket(other.tag_);
        if (a != b) {
            return a <=> b;
        }
        if (a == 0) return std::strong_ordering::equal; // both Null
        if (a == 1) {
            return std::get<bool>(storage_) <=> std::get<bool>(other.storage_);
        }
        if (a == 2) {
            const double av = asNumericDouble();
            const double bv = other.asNumericDouble();
            if (av < bv) return std::strong_ordering::less;
            if (av > bv) return std::strong_ordering::greater;
            return std::strong_ordering::equal;
        }
        if (a == 3) {
            const std::string& sa = std::get<std::string>(storage_);
            const std::string& sb = std::get<std::string>(other.storage_);
            if (sa < sb) return std::strong_ordering::less;
            if (sa > sb) return std::strong_ordering::greater;
            return std::strong_ordering::equal;
        }
        // a == 4 (Blob)
        const auto& va = std::get<std::vector<std::uint8_t>>(storage_);
        const auto& vb = std::get<std::vector<std::uint8_t>>(other.storage_);
        if (va < vb) return std::strong_ordering::less;
        if (va > vb) return std::strong_ordering::greater;
        return std::strong_ordering::equal;
    }

    // `operator==` is tag-true equality (different-tag values are never equal).
    bool operator==(const Value& other) const {
        if (tag_ != other.tag_) return false;
        switch (tag_) {
            case ValueType::Null:      return true;
            case ValueType::Bool:      return asBool() == other.asBool();
            case ValueType::Int32:     return asInt32() == other.asInt32();
            case ValueType::Int64:     return asInt64() == other.asInt64();
            case ValueType::Double:    return asDouble() == other.asDouble();
            case ValueType::Text:      return asText() == other.asText();
            case ValueType::Blob:      return asBlob() == other.asBlob();
            case ValueType::Date:      return asDate() == other.asDate();
            case ValueType::Timestamp: return asTimestamp() == other.asTimestamp();
        }
        return false;
    }

    // All relational comparisons derived from `compare`.
    bool operator!=(const Value& other) const { return compare(other) != 0; }
    bool operator< (const Value& other) const { return compare(other) <  0; }
    bool operator<=(const Value& other) const { return compare(other) <= 0; }
    bool operator> (const Value& other) const { return compare(other) >  0; }
    bool operator>=(const Value& other) const { return compare(other) >= 0; }

    // ---- pretty-print ---------------------------------------------------------
    std::string toString() const {
        switch (tag_) {
            case ValueType::Null:      return "NULL";
            case ValueType::Bool:      return asBool() ? "true" : "false";
            case ValueType::Int32:     return std::to_string(asInt32());
            case ValueType::Int64:     return std::to_string(asInt64());
            case ValueType::Double: {
                std::string s = std::to_string(asDouble());
                if (s.find('.') != std::string::npos) {
                    std::size_t last = s.find_last_not_of('0');
                    if (s[last] == '.') --last;
                    s.erase(last + 1);
                }
                return s;
            }
            case ValueType::Text:      return asText();
            case ValueType::Blob:      return "<blob:" + std::to_string(asBlob().size()) + "B>";
            case ValueType::Date:      return dateToIso(asDate());
            case ValueType::Timestamp: return timestampToIso(asTimestamp());
        }
        return std::string{}; // unreachable
    }

    friend std::ostream& operator<<(std::ostream& os, const Value& v) {
        return os << v.toString();
    }

    // ---- binary (de)serialization for durable storage -------------------------
    // Wire format (version 1):
    //   [1 byte: tag]   [variable payload]
    // Payloads:
    //   Null      — no payload
    //   Bool      — 1 byte (0 or 1)
    //   Int32     — 4 bytes little-endian
    //   Int64     — 8 bytes little-endian
    //   Double    — 8 bytes IEEE 754 little-endian (memcpy)
    //   Text      — [4 bytes LE length] [N bytes UTF-8]
    //   Blob      — [4 bytes LE length] [N raw bytes]
    //   Date      — 4 bytes LE (same as Int32)
    //   Timestamp — 8 bytes LE (same as Int64)
    std::vector<std::uint8_t> toBytes() const {
        std::vector<std::uint8_t> out;
        out.push_back(static_cast<std::uint8_t>(tag_));
        switch (tag_) {
            case ValueType::Null:
                break;
            case ValueType::Bool:
                out.push_back(asBool() ? 1u : 0u);
                break;
            case ValueType::Int32:
            case ValueType::Date: {
                auto i = static_cast<std::uint32_t>(asInt32());
                for (int b = 0; b < 4; ++b) out.push_back(static_cast<std::uint8_t>(i >> (8 * b)));
                break;
            }
            case ValueType::Int64:
            case ValueType::Timestamp: {
                auto i = static_cast<std::uint64_t>(asInt64());
                for (int b = 0; b < 8; ++b) out.push_back(static_cast<std::uint8_t>(i >> (8 * b)));
                break;
            }
            case ValueType::Double: {
                std::uint64_t bits;
                double d = asDouble();
                std::memcpy(&bits, &d, sizeof(bits));
                for (int b = 0; b < 8; ++b) out.push_back(static_cast<std::uint8_t>(bits >> (8 * b)));
                break;
            }
            case ValueType::Text: {
                const auto& s = asText();
                auto len = static_cast<std::uint32_t>(s.size());
                for (int b = 0; b < 4; ++b) out.push_back(static_cast<std::uint8_t>(len >> (8 * b)));
                out.insert(out.end(), s.begin(), s.end());
                break;
            }
            case ValueType::Blob: {
                const auto& v = asBlob();
                auto len = static_cast<std::uint32_t>(v.size());
                for (int b = 0; b < 4; ++b) out.push_back(static_cast<std::uint8_t>(len >> (8 * b)));
                out.insert(out.end(), v.begin(), v.end());
                break;
            }
        }
        return out;
    }

    // Parse a Value from the wire format written by toBytes(). `iter` is advanced
    // past the consumed bytes. Throws std::runtime_error on malformed input.
    static Value fromBytes(std::vector<std::uint8_t>::const_iterator& iter) {
        if (iter == std::vector<std::uint8_t>{}.end())
            throw std::runtime_error("fromBytes: empty input");
        auto tag = static_cast<ValueType>(*iter++);
        switch (tag) {
            case ValueType::Null:
                return Value::null();
            case ValueType::Bool: {
                bool b = (*iter != 0u); ++iter;
                return Value::boolean(b);
            }
            case ValueType::Int32:
            case ValueType::Date: {
                std::uint32_t i = 0;
                for (int b = 0; b < 4; ++b) i |= static_cast<std::uint32_t>(*iter++) << (8 * b);
                if (tag == ValueType::Date)
                    return Value::date(static_cast<std::int32_t>(i));
                return Value::int32(static_cast<std::int32_t>(i));
            }
            case ValueType::Int64:
            case ValueType::Timestamp: {
                std::uint64_t i = 0;
                for (int b = 0; b < 8; ++b) i |= static_cast<std::uint64_t>(*iter++) << (8 * b);
                if (tag == ValueType::Timestamp)
                    return Value::timestamp(static_cast<std::int64_t>(i));
                return Value::int64(static_cast<std::int64_t>(i));
            }
            case ValueType::Double: {
                std::uint64_t bits = 0;
                for (int b = 0; b < 8; ++b) bits |= static_cast<std::uint64_t>(*iter++) << (8 * b);
                double d;
                std::memcpy(&d, &bits, sizeof(d));
                return Value::real(d);
            }
            case ValueType::Text: {
                std::uint32_t len = 0;
                for (int b = 0; b < 4; ++b) len |= static_cast<std::uint32_t>(*iter++) << (8 * b);
                std::string s(iter, iter + len);
                iter += len;
                return Value::text(std::move(s));
            }
            case ValueType::Blob: {
                std::uint32_t len = 0;
                for (int b = 0; b < 4; ++b) len |= static_cast<std::uint32_t>(*iter++) << (8 * b);
                std::vector<std::uint8_t> v(iter, iter + len);
                iter += len;
                return Value::blob(std::move(v));
            }
        }
        throw std::runtime_error("fromBytes: unknown tag");
    }

private:
    double asNumericDouble() const {
        switch (tag_) {
            case ValueType::Int32:     return static_cast<double>(asInt32());
            case ValueType::Int64:     return static_cast<double>(asInt64());
            case ValueType::Double:    return asDouble();
            case ValueType::Date:      return static_cast<double>(asDate());
            case ValueType::Timestamp:  return static_cast<double>(asTimestamp());
            default: return 0.0;
        }
    }

    // ISO 8601 helpers for Date/Timestamp printing.
    static std::string dateToIso(std::int32_t days) {
        // Days since 1970-01-01 -> YYYY-MM-DD. Algorithm from Howard Hinnant's
        // date library (public domain): https://howardhinnant.github.io/date_algorithms.html
        std::int32_t z = days + 719468;  // days since 0000-03-01
        std::int32_t era = (z >= 0 ? z : z - 146096) / 146097;
        std::uint32_t doe = static_cast<std::uint32_t>(z - era * 146097);           // [0, 146096]
        std::uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;        // [0, 399]
        std::int32_t y = static_cast<std::int32_t>(yoe) + era * 400;
        std::uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);                       // [0, 365]
        std::uint32_t mp = (5*doy + 2)/153;                                          // [0, 11]
        std::uint32_t d = doy - (153*mp+2)/5 + 1;                                    // [1, 31]
        std::uint32_t m = mp < 10 ? mp + 3 : mp - 9;                                // [1, 12]
        if (m <= 2) y += 1;  // year correction for proleptic Gregorian
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
        return buf;
    }
    static std::string timestampToIso(std::int64_t millis) {
        std::int64_t sec = millis / 1000;
        std::int64_t ms  = millis % 1000;
        if (ms < 0) { ms += 1000; sec -= 1; }
        std::int32_t days = static_cast<std::int32_t>(sec / 86400);
        std::int64_t rem  = sec % 86400;
        if (rem < 0) { rem += 86400; days -= 1; }
        std::int32_t hh = static_cast<std::int32_t>(rem / 3600);
        std::int32_t mm = static_cast<std::int32_t>((rem / 60) % 60);
        std::int32_t ss = static_cast<std::int32_t>(rem % 60);
        std::string date_part = dateToIso(days);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "T%02d:%02d:%02d.%03lldZ",
                      hh, mm, ss, static_cast<long long>(ms));
        return date_part + buf;
    }

    // StorageLayout: monostate(Null), bool, int32, int64, double, string, vector<uint8_t>.
    // Date reuses the int32 slot; Timestamp reuses int64; Blob uses vector.
    using Storage = std::variant<std::monostate, bool,
                                  std::int32_t, std::int64_t,
                                  double, std::string,
                                  std::vector<std::uint8_t>>;
    Storage storage_;
    ValueType tag_ = ValueType::Null;
};

} // namespace opendb

#endif // OPENDB_VALUE_HPP
