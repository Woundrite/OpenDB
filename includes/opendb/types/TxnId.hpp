#ifndef OPENDB_TXN_ID_HPP
#define OPENDB_TXN_ID_HPP

#include <cstdint>
#include <functional>
#include <string>

namespace opendb {

// TxnId: a strongly-typed transaction handle (spec §3.2). Distinct from raw
// integers via `explicit` constructor — no accidental arithmetic or mixing with
// page ids, table ids, etc. Acts as a key into the TransactionManager and as
// the principal argument to the LockManager and StorageEngine contracts.
class TxnId {
public:
    explicit TxnId(std::uint64_t v) noexcept : value_(v) {}

    std::uint64_t value() const noexcept { return value_; }

    std::string toString() const { return "Txn" + std::to_string(value_); }

    bool operator==(const TxnId&) const = default;
    std::strong_ordering operator<=>(const TxnId& other) const {
        return value_ <=> other.value_;
    }

private:
    std::uint64_t value_;
};

} // namespace opendb

template <>
struct std::hash<opendb::TxnId> {
    std::size_t operator()(const opendb::TxnId& t) const noexcept {
        return std::hash<std::uint64_t>{}(t.value());
    }
};

#endif // OPENDB_TXN_ID_HPP
