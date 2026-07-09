#ifndef ATOMDB_COLUMN_HPP
#define ATOMDB_COLUMN_HPP

#include <string>

#include "atomdb/types/Value.hpp"

namespace atomdb {

// A single (column name, Value) pair (spec §3.2).
struct ColumnValue {
    std::string name;
    Value value;

    bool operator==(const ColumnValue&) const = default;
};

} // namespace atomdb

#endif // ATOMDB_COLUMN_HPP
