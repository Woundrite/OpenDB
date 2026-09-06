#ifndef OPENDB_COMMAND_HPP
#define OPENDB_COMMAND_HPP

#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "opendb/types/Predicate.hpp"
#include "opendb/types/Result.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/TxnId.hpp"

namespace opendb {

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
// Sort direction for ORDER BY clause.
enum class SortDirection { Asc, Desc };

// Order-by spec: pair of (column, direction). Order is meaningful — first
// entry sorts first, then ties on second, etc.
//
// nullsFirst: when std::nullopt, defaults follow PostgreSQL: ASC = NULLS LAST,
// DESC = NULLS FIRST. When set, the user-specified ordering wins.
//   - nullsFirst = true  -> NULLs sort before non-NULLs
//   - nullsFirst = false -> NULLs sort after non-NULLs
struct OrderBySpec {
    std::string column;
    SortDirection direction = SortDirection::Asc;
    std::optional<bool> nullsFirst = std::nullopt;
};

// Phase 6.2: JOIN support.
//
// JoinKind controls whether unjoined left rows are preserved (Left) or
// dropped (Inner). JoinClause carries the right table plus the single-
// equality ON predicate expressed as two fully-qualified columns
// ("leftTable.col" / "rightTable.col"). The EngineLoop nested-loop executor
// resolves those qualifiers against the merged tuple at join time.
enum class JoinKind { Inner, Left };

struct JoinClause {
    JoinKind kind = JoinKind::Inner;
    std::string table;
    std::string leftColumn;   // "a.id"
    std::string rightColumn;  // "b.user_id"
};

class Command {
public:
    CommandType type;
    std::string table;
    std::optional<Predicate> where;
    std::optional<Tuple> values;
    std::vector<std::string> projections;
    std::vector<OrderBySpec> orderBy;
    std::vector<JoinClause> joins;  // Phase 6.2
    std::optional<std::size_t> limit;
    std::optional<std::size_t> offset;
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

    // ponytail: ORDER BY/LIMIT/OFFSET are set after construction rather
    // than added to the ctor param list — keeps existing 4/6-arg call
    // sites compiling unchanged while still letting SqlParser populate
    // them atomically via this setter chain.
    Command& withOrderBy(std::vector<OrderBySpec> v) { orderBy = std::move(v); return *this; }
    Command& withLimit(std::size_t v) { limit = v; return *this; }
    Command& withOffset(std::size_t v) { offset = v; return *this; }
    Command& withJoin(JoinClause j) { joins.push_back(std::move(j)); return *this; }

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
        if (!orderBy.empty()) {
            os << " orderBy=[";
            for (std::size_t i = 0; i < orderBy.size(); ++i) {
                if (i) os << ',';
                os << orderBy[i].column
                   << (orderBy[i].direction == SortDirection::Asc ? ":asc" : ":desc");
                if (orderBy[i].nullsFirst.has_value()) {
                    os << (*orderBy[i].nullsFirst ? ":nullsFirst" : ":nullsLast");
                }
            }
            os << "]";
        }
        if (limit)  os << " limit=" << *limit;
        if (offset) os << " offset=" << *offset;
        if (!joins.empty()) {
            os << " joins=[";
            for (std::size_t i = 0; i < joins.size(); ++i) {
                if (i) os << ',';
                os << (joins[i].kind == JoinKind::Inner ? "Inner" : "Left")
                   << ':' << joins[i].table
                   << ':' << joins[i].leftColumn
                   << '=' << joins[i].rightColumn;
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

} // namespace opendb

#endif // OPENDB_COMMAND_HPP
