#ifndef OPENDB_COLUMN_HPP
#define OPENDB_COLUMN_HPP

#include <string>

#include "opendb/types/Value.hpp"

namespace opendb {

// A single (column name, Value) pair (spec §3.2).
struct ColumnValue {
    std::string name;
    Value value;

    bool operator==(const ColumnValue&) const = default;
};

} // namespace opendb

#endif // OPENDB_COLUMN_HPP
