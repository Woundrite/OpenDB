#ifndef ATOMDB_TUPLE_HPP
#define ATOMDB_TUPLE_HPP

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atomdb/types/Column.hpp"
#include "atomdb/types/Value.hpp"

namespace atomdb {

// Tuple: a full row — ordered, named Values with O(1) lookup by column name
// (spec §3.2). Both storage records and query results are Tuples. Equality is
// order-sensitive: two Tuples are equal iff they carry the same columns in the
// same order with equal Values.
class Tuple {
public:
    using const_iterator = std::vector<ColumnValue>::const_iterator;

    Tuple() = default;

    Tuple(std::initializer_list<ColumnValue> cols) {
        columns_.reserve(cols.size());
        for (const auto& c : cols) {
            index_.emplace(c.name, columns_.size());
            columns_.push_back(c);
        }
    }

    explicit Tuple(std::vector<ColumnValue> cols)
        : columns_(std::move(cols)) {
        index_.reserve(columns_.size());
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            index_.emplace(columns_[i].name, i);
        }
    }

    // Convenience factory: Tuple::make({{"age", Value::int32(30)}, ...}).
    static Tuple make(std::initializer_list<ColumnValue> cols) {
        return Tuple(cols);
    }

    const std::vector<ColumnValue>& columns() const noexcept { return columns_; }

    std::size_t size() const noexcept { return columns_.size(); }
    bool empty() const noexcept { return columns_.empty(); }

    bool has(const std::string& name) const {
        return index_.find(name) != index_.end();
    }

    const Value& get(const std::string& name) const {
        auto it = index_.find(name);
        if (it == index_.end()) {
            throw std::out_of_range("Tuple: missing column '" + name + "'");
        }
        return columns_[it->second].value;
    }

    std::optional<Value> maybeGet(const std::string& name) const {
        auto it = index_.find(name);
        if (it == index_.end()) return std::nullopt;
        return columns_[it->second].value;
    }

    // `set` on an existing column replaces the value in-place (preserving order);
    // `set` on a new column appends at the end (updating the index). This is the
    // mutation primitive used by the storage engine and tests, but the tuple itself
    // does not carry version metadata — versioning lives in the storage layer.
    void set(const std::string& name, Value value) {
        auto it = index_.find(name);
        if (it != index_.end()) {
            columns_[it->second].value = std::move(value);
        } else {
            index_.emplace(name, columns_.size());
            columns_.push_back({name, std::move(value)});
        }
    }

    const_iterator begin() const noexcept { return columns_.begin(); }
    const_iterator end()   const noexcept { return columns_.end(); }

    bool operator==(const Tuple& other) const {
        return columns_ == other.columns_;
    }

    std::string toString() const {
        std::string s;
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            if (i) s += ',';
            s += columns_[i].name;
            s += '=';
            s += columns_[i].value.toString();
        }
        return s;
    }

private:
    std::vector<ColumnValue> columns_;
    std::unordered_map<std::string, std::size_t> index_;
};

inline std::ostream& operator<<(std::ostream& os, const Tuple& t) {
    return os << t.toString();
}

} // namespace atomdb

#endif // ATOMDB_TUPLE_HPP
