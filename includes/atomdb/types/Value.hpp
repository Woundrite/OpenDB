#ifndef ATOMDB_VALUE_HPP
#define ATOMDB_VALUE_HPP

#include <cstdint>
#include <ostream>
#include <string>
#include <variant>

// Value: the tagged-union scalar of AtomDB (spec §3.2).
//
// Six tags: Null, Bool, Int32, Int64, Double, Text.
// Notes:
//   - Int32 and Int64 are kept distinct in the variant for type fidelity, but
//     `compare()` orders them numerically as a combined Number category.
//     Consequently Int32(5).compare(Int64(5)) == std::strong_ordering::equal.
//   - `operator==` is tag-true equality: two Values are equal iff they share
//     the exact same tag and the same payload. So Int32(5) == Int64(5) is *false*
//     even though their ordering is the same. Bool(true) == Int64(1) is false.
//   - Double(5.0).compare(Int32(5)) compares *numerically* (equal when the
//     double's value is exactly integral and equals the integer). This is the
//     v0.1 simplification noted in the task write-up; for a SQL-style engine
//     this is the most ergonomic default.
//
// Ordering categories:
//   Null  <  Bool  <  Number(Int32|Int64|Double)  <  Text
// Within Bool: false < true.
// Within Number: numeric magnitude (negative < positive; same magnitude equal).
// Within Text: std::string lexicographic byte comparison.

namespace atomdb {

enum class ValueType {
    Null,
    Bool,
    Int32,
    Int64,
    Double,
    Text,
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

    // ---- observations ---------------------------------------------------------
    ValueType type() const noexcept { return tag_; }

    bool isNull()     const noexcept { return tag_ == ValueType::Null; }
    bool isBool()     const noexcept { return tag_ == ValueType::Bool; }
    bool isInt32()    const noexcept { return tag_ == ValueType::Int32; }
    bool isInt64()    const noexcept { return tag_ == ValueType::Int64; }
    bool isDouble()   const noexcept { return tag_ == ValueType::Double; }
    bool isText()     const noexcept { return tag_ == ValueType::Text; }
    bool isNumber()   const noexcept {
        return tag_ == ValueType::Int32 ||
               tag_ == ValueType::Int64 ||
               tag_ == ValueType::Double;
    }

    // UB if type mismatch (documented; no throw for hot-path speed).
    bool          asBool()   const { return std::get<bool>(storage_); }
    std::int32_t  asInt32()  const { return std::get<std::int32_t>(storage_); }
    std::int64_t  asInt64()  const { return std::get<std::int64_t>(storage_); }
    double        asDouble() const { return std::get<double>(storage_); }
    const std::string& asText() const { return std::get<std::string>(storage_); }

    // ---- ordering -------------------------------------------------------------
    // Returns a strong_ordering driving all six comparison operators.
    std::strong_ordering compare(const Value& other) const {
        // Normalize Number categories to the same rank so they compare together.
        const auto num_rank = [](ValueType t) -> int {
            switch (t) {
                case ValueType::Null:   return 0;
                case ValueType::Bool:    return 1;
                case ValueType::Int32:
                case ValueType::Int64:
                case ValueType::Double:  return 2; // shared Number bucket
                case ValueType::Text:   return 3;
            }
            return 4;
        };
        const int a = num_rank(tag_);
        const int b = num_rank(other.tag_);
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
        // a == 3 (Text). Within-tag only; tag differs handled above.
        const std::string& sa = std::get<std::string>(storage_);
        const std::string& sb = std::get<std::string>(other.storage_);
        if (sa < sb) return std::strong_ordering::less;
        if (sa > sb) return std::strong_ordering::greater;
        return std::strong_ordering::equal;
    }

    // `operator==` is tag-true equality (different-tag values are never equal).
    bool operator==(const Value& other) const {
        if (tag_ != other.tag_) return false;
        switch (tag_) {
            case ValueType::Null:   return true;
            case ValueType::Bool:   return asBool() == other.asBool();
            case ValueType::Int32:  return asInt32() == other.asInt32();
            case ValueType::Int64:  return asInt64() == other.asInt64();
            case ValueType::Double: return asDouble() == other.asDouble();
            case ValueType::Text:   return asText() == other.asText();
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
            case ValueType::Null:   return "NULL";
            case ValueType::Bool:   return asBool() ? "true" : "false";
            case ValueType::Int32:  return std::to_string(asInt32());
            case ValueType::Int64:  return std::to_string(asInt64());
            case ValueType::Double: {
                // std::to_string gives 6dp; trim trailing zeros + decimal point.
                std::string s = std::to_string(asDouble());
                if (s.find('.') != std::string::npos) {
                    std::size_t last = s.find_last_not_of('0');
                    if (s[last] == '.') --last;
                    s.erase(last + 1);
                }
                return s;
            }
            case ValueType::Text:   return asText();
        }
        return std::string{}; // unreachable
    }

    friend std::ostream& operator<<(std::ostream& os, const Value& v) {
        return os << v.toString();
    }

private:
    double asNumericDouble() const {
        switch (tag_) {
            case ValueType::Int32:  return static_cast<double>(asInt32());
            case ValueType::Int64:  return static_cast<double>(asInt64());
            case ValueType::Double: return asDouble();
            default: return 0.0; // unused for non-Number branches; caller guards rank.
        }
    }

    using Storage = std::variant<std::monostate, bool,
                                  std::int32_t, std::int64_t,
                                  double, std::string>;
    Storage storage_;
    ValueType tag_ = ValueType::Null;
};

} // namespace atomdb

#endif // ATOMDB_VALUE_HPP
