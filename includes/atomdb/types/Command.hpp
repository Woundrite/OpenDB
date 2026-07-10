#ifndef ATOMDB_COMMAND_HPP
#define ATOMDB_COMMAND_HPP

#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "atomdb/types/Predicate.hpp"
#include "atomdb/types/Result.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/TxnId.hpp"

namespace atomdb {

// Command: the universal IR crossing the front-end -> core boundary (spec §3.2).
// The only object a front-end may hand to the core engine; no front-end may pass
// raw SQL / JSON past this point.

enum class CommandType { Select, Insert, Update, Delete };

// Invariants enforced in the constructor (throw std::invalid_argument on
// violation). A parser that produces an invalid combination is buggy; we'd
// rather fail loud here than dispatch to Undefined Behaviour downstream.
//   - Insert: values MUST be set, where MUST NOT be set (no WHERE on INSERT in v0.1).
//   - Update: MUST set where (must identify what to mutate) and values.
//   - Delete: MUST set where, MUST NOT set values.
//   - Select: optional where (filter), optional projections (column subset).
//   - txnId: may be set (explicit txn) or nullopt (auto-commit).
class Command {
public:
    CommandType type;
    std::string table;
    std::optional<Predicate> where;
    std::optional<Tuple> values;
    std::vector<std::string> projections;
    std::optional<TxnId> txnId;

    // Construct in-place. Move-only-friendly — pass rvalue Tuple/Predicate.
    Command(CommandType t,
            std::string tbl,
            std::optional<Predicate> wh = std::nullopt,
            std::optional<Tuple> vals = std::nullopt,
            std::vector<std::string> projections = {},
            std::optional<TxnId> txn = std::nullopt)
        : type(t),
          table(std::move(tbl)),
          where(std::move(wh)),
          values(std::move(vals)),
          projections(std::move(projections)),
          txnId(txn) {
        validate();
    }

    std::string toString() const {
        const char* type_str = "?";
        switch (type) {
            case CommandType::Select: type_str = "Select"; break;
            case CommandType::Insert: type_str = "Insert"; break;
            case CommandType::Update: type_str = "Update"; break;
            case CommandType::Delete: type_str = "Delete"; break;
        }
        std::ostringstream os;
        os << type_str << " table=" << table;
        if (where)   os << " where=" << where->toString();
        if (values)  os << " values=" << values->toString();
        if (!projections.empty()) {
            os << " proj=[";
            for (std::size_t i = 0; i < projections.size(); ++i) {
                if (i) os << ',';
                os << projections[i];
            }
            os << "]";
        }
        if (txnId)   os << " txn=" << txnId->toString();
        return os.str();
    }

private:
    void validate() const {
        switch (type) {
            case CommandType::Insert: {
                if (!values) throw std::invalid_argument("Insert requires values");
                if (where)   throw std::invalid_argument("Insert must not carry WHERE");
                break;
            }
            case CommandType::Update: {
                if (!where)  throw std::invalid_argument("Update requires WHERE");
                if (!values) throw std::invalid_argument("Update requires values");
                break;
            }
            case CommandType::Delete: {
                if (!where)   throw std::invalid_argument("Delete requires WHERE");
                if (values)   throw std::invalid_argument("Delete must not carry values");
                break;
            }
            case CommandType::Select:
                break;
        }
    }
};

} // namespace atomdb

#endif // ATOMDB_COMMAND_HPP
