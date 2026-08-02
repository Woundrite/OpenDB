#ifndef ATOMDB_SQL_PARSER_HPP
#define ATOMDB_SQL_PARSER_HPP

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "atomdb/types/Command.hpp"
#include "atomdb/types/Schema.hpp"

namespace atomdb {

// SqlStatement: output from parsing one SQL statement.
//
// DDL bypasses the Command IR (which only covers Select/Insert/Update/Delete) and
// lands directly as a DdlCommand carrying a Schema. The dispatch path is the
// front-end's responsibility — typically CREATE/DROP TABLE go straight to the
// IStorageProvider, while DML statements hand their Command back into the
// core EngineLoop.
//
// Transaction-control statements (BEGIN / COMMIT / ROLLBACK) are also
// represented separately because the Command IR validates that Insert requires
// values and Delete requires where — neither fits a bare BEGIN/COMMIT. The
// SqlParser emits SqlTxnControl for these; the front-end dispatches them
// directly to TransactionManager instead of EngineLoop.
struct DdlCreateTable {
    Schema schema;
};
struct DdlDropTable {
    std::string table;
};
// Phase 6.1: ALTER TABLE statement. Carries the table name and an
// AlterSpec that the provider's alterTable() applies.
struct DdlAlterTable {
    std::string table;
    AlterSpec spec;
};
struct SqlTxnBegin {};
struct SqlTxnCommit {};
struct SqlTxnRollback {};

using SqlStatement = std::variant<
    DdlCreateTable,
    DdlDropTable,
    DdlAlterTable,
    SqlTxnBegin,
    SqlTxnCommit,
    SqlTxnRollback,
    Command
>;

// SqlParser: hand-rolled recursive-descent parser for a small SQL subset.
//
// Supported:
//   CREATE TABLE foo (id INT PRIMARY KEY, name TEXT, age INT)
//   CREATE TABLE foo (...) PARTITION BY HASH(id) PARTITIONS 4
//   DROP TABLE foo
//   INSERT INTO foo [cols] VALUES (val,val,val)        -- cols optional
//   SELECT [* | col,col] FROM foo [WHERE <pred>]
//   UPDATE foo SET col=val,[col=val] [WHERE <pred>]
//   DELETE FROM foo [WHERE <pred>]
//   BEGIN [TRANSACTION]
//   COMMIT
//   ROLLBACK
//
// Where <pred>:
//   <col> <op> <value> [AND | OR <pred>]
//   <op> := = | != | < | <= | > | >=
//
// Values:
//   INT literal -> Value::int64
//   FLOAT literal -> Value::double
//   Quoted string ('...' or "...") -> Value::text
//   true / false -> Value::boolean
//   null -> Value::null
//
// Returns std::nullopt on parse error; the error message is available via
// error() for the caller to present. Other empty statements (only comments /
// whitespace) also return nullopt.
class SqlParser {
public:
    std::optional<SqlStatement> parse(const std::string& sql);
    const std::string& error() const noexcept { return error_; }

    // Convenience: parse a multi-statement string into a vector. Statements
    // are separated by ';' outside of string literals. Semicolon is optional
    // on the last statement.
    std::vector<SqlStatement> parseAll(const std::string& sql);

private:
    std::string input_;
    std::size_t pos_{0};
    std::string error_;
    int stmtNumber_{0};

    // tokenizer helpers
    void skipWhitespaceAndComments();
    bool consumeKeyword(const std::string& kw);
    bool consumeSymbol(const std::string& sym);
    std::optional<std::string> consumeIdentifier();
    std::optional<std::string> consumeWord();
    std::optional<std::string> consumeStringLiteral();
    std::optional<Value> consumeValueLiteral();

    // SQL grammar
    std::optional<SqlStatement> parseStatement();
    std::optional<DdlCreateTable> parseCreateTable();
    std::optional<DdlDropTable>  parseDropTable();
    std::optional<DdlAlterTable> parseAlterTable();
    std::optional<Command> parseInsert();
    std::optional<Command> parseSelect();
    std::optional<Command> parseUpdate();
    std::optional<Command> parseDelete();
    std::optional<Command> parseBegin();
    std::optional<Command> parseCommit();
    std::optional<Command> parseRollback();

    // Expressions
    std::unique_ptr<PredicateNode> parsePredicate();
    std::unique_ptr<PredicateNode> parseOrExpr();
    std::unique_ptr<PredicateNode> parseAndExpr();
    std::unique_ptr<PredicateNode> parsePrimExpr();

    std::optional<ComparisonOp> parseComparisonOp();

    // Column spec for CREATE TABLE
    bool parseColumnSpec(ColumnDef& out, bool& hasPrimaryKey);

    // Pieces
    Tuple parseInsertValueList();
    std::vector<ColumnValue> parseColumnValuePairs();
};

} // namespace atomdb

#endif // ATOMDB_SQL_PARSER_HPP