#include "atomdb/frontend/SqlParser.hpp"
#include "atomdb/types/Value.hpp"
#include "atomdb/types/Predicate.hpp"
#include "atomdb/types/Schema.hpp"
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <variant>

namespace atomdb {

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
void SqlParser::skipWhitespaceAndComments() {
    while (pos_ < input_.size()) {
        if (std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
            continue;
        }
        // -- comment to end of line
        if (pos_ + 1 < input_.size() && input_[pos_] == '-' && input_[pos_ + 1] == '-') {
            pos_ += 2;
            while (pos_ < input_.size() && input_[pos_] != '\n') ++pos_;
            continue;
        }
        // /* ... */ block comment
        if (pos_ + 1 < input_.size() && input_[pos_] == '/' && input_[pos_ + 1] == '*') {
            pos_ += 2;
            while (pos_ + 1 < input_.size() && !(input_[pos_] == '*' && input_[pos_ + 1] == '/')) ++pos_;
            if (pos_ + 1 < input_.size()) pos_ += 2;
            continue;
        }
        break;
    }
}

bool SqlParser::consumeKeyword(const std::string& kw) {
    skipWhitespaceAndComments();
    if (pos_ + kw.size() > input_.size()) return false;
    // Case-insensitive match
    for (std::size_t i = 0; i < kw.size(); ++i) {
        char a = input_[pos_ + i];
        char b = kw[i];
        if (std::toupper(static_cast<unsigned char>(a)) != std::toupper(static_cast<unsigned char>(b))) return false;
    }
    // Word boundary check: next char must not be alphanumeric/_
    if (pos_ + kw.size() < input_.size()) {
        char c = input_[pos_ + kw.size()];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') return false;
    }
    pos_ += kw.size();
    return true;
}

bool SqlParser::consumeSymbol(const std::string& sym) {
    skipWhitespaceAndComments();
    if (pos_ + sym.size() > input_.size()) return false;
    if (input_.compare(pos_, sym.size(), sym) != 0) return false;
    pos_ += sym.size();
    return true;
}

std::optional<std::string> SqlParser::consumeIdentifier() {
    skipWhitespaceAndComments();
    if (pos_ >= input_.size()) return std::nullopt;
    // Quoted identifier "foo"
    if (input_[pos_] == '"') {
        ++pos_;
        std::size_t start = pos_;
        while (pos_ < input_.size() && input_[pos_] != '"') ++pos_;
        if (pos_ >= input_.size()) {
            error_ = "unterminated quoted identifier";
            return std::nullopt;
        }
        std::string id = input_.substr(start, pos_ - start);
        ++pos_;
        return id;
    }
    // Bare identifier: letter/underscore followed by alnum/underscore
    char c = input_[pos_];
    if (!std::isalpha(static_cast<unsigned char>(c)) && c != '_') return std::nullopt;
    std::size_t start = pos_;
    while (pos_ < input_.size() &&
           (std::isalnum(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '_')) {
        ++pos_;
    }
    return input_.substr(start, pos_ - start);
}

std::optional<std::string> SqlParser::consumeWord() {
    skipWhitespaceAndComments();
    if (pos_ >= input_.size()) return std::nullopt;
    std::size_t start = pos_;
    while (pos_ < input_.size() &&
           (std::isalnum(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '_')) {
        ++pos_;
    }
    if (pos_ == start) return std::nullopt;
    return input_.substr(start, pos_ - start);
}

std::optional<std::string> SqlParser::consumeStringLiteral() {
    skipWhitespaceAndComments();
    if (pos_ >= input_.size()) return std::nullopt;
    char quote = input_[pos_];
    if (quote != '\'' && quote != '"') return std::nullopt;
    ++pos_;
    std::size_t start = pos_;
    while (pos_ < input_.size() && input_[pos_] != quote) {
        if (input_[pos_] == '\\' && pos_ + 1 < input_.size()) ++pos_; // skip escaped
        ++pos_;
    }
    if (pos_ >= input_.size()) {
        error_ = "unterminated string literal";
        return std::nullopt;
    }
    std::string s = input_.substr(start, pos_ - start);
    ++pos_;
    return s;
}

std::optional<Value> SqlParser::consumeValueLiteral() {
    skipWhitespaceAndComments();
    if (pos_ >= input_.size()) return std::nullopt;

    // String literal
    if (input_[pos_] == '\'' || input_[pos_] == '"') {
        auto s = consumeStringLiteral();
        if (!s) return std::nullopt;
        return Value::text(*s);
    }

    // true / false / null
    if (consumeKeyword("TRUE")) return Value::boolean(true);
    if (consumeKeyword("FALSE")) return Value::boolean(false);
    if (consumeKeyword("NULL")) return Value::null();

    // Number (int or float)
    std::size_t start = pos_;
    bool hasDot = false;
    bool hasExp = false;
    if (input_[pos_] == '+' || input_[pos_] == '-') ++pos_;
    while (pos_ < input_.size()) {
        char c = input_[pos_];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            ++pos_;
        } else if (c == '.' && !hasDot && !hasExp) {
            hasDot = true;
            ++pos_;
        } else if ((c == 'e' || c == 'E') && !hasExp) {
            hasExp = true;
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
        } else {
            break;
        }
    }
    if (pos_ > start) {
        std::string numStr = input_.substr(start, pos_ - start);
        if (hasDot || hasExp) {
            char* end;
            double d = std::strtod(numStr.c_str(), &end);
            return Value::real(d);
        } else {
            char* end;
            long long ll = std::strtoll(numStr.c_str(), &end, 10);
            return Value::int64(ll);
        }
    }

    error_ = "expected value literal at position " + std::to_string(pos_);
    return std::nullopt;
}

// -----------------------------------------------------------------------------
// Statement parsing
// -----------------------------------------------------------------------------
std::optional<SqlStatement> SqlParser::parse(const std::string& sql) {
    input_ = sql;
    pos_ = 0;
    error_.clear();
    ++stmtNumber_;
    return parseStatement();
}

std::vector<SqlStatement> SqlParser::parseAll(const std::string& sql) {
    std::vector<SqlStatement> out;
    std::size_t start = 0;
    std::size_t len = sql.size();
    bool inSingle = false, inDouble = false;

    for (std::size_t i = 0; i <= len; ++i) {
        char c = (i < len) ? sql[i] : ';';
        if (c == '\'' && !inDouble) inSingle = !inSingle;
        else if (c == '"' && !inSingle) inDouble = !inDouble;
        else if (c == ';' && !inSingle && !inDouble) {
            std::string stmt = sql.substr(start, i - start);
            if (auto parsed = parse(stmt)) out.push_back(*parsed);
            start = i + 1;
        }
    }
    if (start < len) {
        std::string stmt = sql.substr(start);
        if (auto parsed = parse(stmt)) out.push_back(*parsed);
    }
    return out;
}

std::optional<SqlStatement> SqlParser::parseStatement() {
    skipWhitespaceAndComments();
    if (pos_ >= input_.size()) {
        error_ = "empty statement";
        return std::nullopt;
    }

    // CREATE TABLE
    if (consumeKeyword("CREATE")) {
        if (consumeKeyword("TABLE")) return parseCreateTable();
        error_ = "expected TABLE after CREATE";
        return std::nullopt;
    }
    // DROP TABLE
    if (consumeKeyword("DROP")) {
        if (consumeKeyword("TABLE")) return parseDropTable();
        error_ = "expected TABLE after DROP";
        return std::nullopt;
    }
    // INSERT
    if (consumeKeyword("INSERT")) return parseInsert();
    // SELECT
    if (consumeKeyword("SELECT")) return parseSelect();
    // UPDATE
    if (consumeKeyword("UPDATE")) return parseUpdate();
    // DELETE
    if (consumeKeyword("DELETE")) return parseDelete();
    // BEGIN / START TRANSACTION
    if (consumeKeyword("BEGIN")) {
        // optional TRANSACTION
        consumeKeyword("TRANSACTION");
        return SqlTxnBegin{};
    }
    // COMMIT
    if (consumeKeyword("COMMIT")) {
        return SqlTxnCommit{};
    }
    // ROLLBACK
    if (consumeKeyword("ROLLBACK")) {
        return SqlTxnRollback{};
    }

    error_ = "unknown statement type";
    return std::nullopt;
}

std::optional<DdlCreateTable> SqlParser::parseCreateTable() {
    DdlCreateTable ddl;
    auto tableName = consumeIdentifier();
    if (!tableName) {
        error_ = "expected table name after CREATE TABLE";
        return std::nullopt;
    }
    ddl.schema.table = *tableName;

    if (!consumeSymbol("(")) {
        error_ = "expected '(' after table name";
        return std::nullopt;
    }

    bool hasPrimaryKey = false;
    while (true) {
        skipWhitespaceAndComments();
        if (consumeSymbol(")")) break;

        ColumnDef col;
        if (!parseColumnSpec(col, hasPrimaryKey)) return std::nullopt;
        ddl.schema.columns.push_back(col);

        skipWhitespaceAndComments();
        if (consumeSymbol(",")) continue;
        if (consumeSymbol(")")) break;
        error_ = "expected ',' or ')' in column list";
        return std::nullopt;
    }

    // Optional PARTITION BY
    if (consumeKeyword("PARTITION")) {
        if (!consumeKeyword("BY")) {
            error_ = "expected BY after PARTITION";
            return std::nullopt;
        }
        if (!consumeKeyword("HASH")) {
            error_ = "only HASH partitioning supported";
            return std::nullopt;
        }
        if (!consumeSymbol("(")) {
            error_ = "expected '(' after HASH";
            return std::nullopt;
        }
        auto colName = consumeIdentifier();
        if (!colName) {
            error_ = "expected partition column name";
            return std::nullopt;
        }
        if (!consumeSymbol(")")) {
            error_ = "expected ')' after partition column";
            return std::nullopt;
        }
        if (!consumeKeyword("PARTITIONS")) {
            error_ = "expected PARTITIONS N";
            return std::nullopt;
        }
        auto countStr = consumeWord();
        if (!countStr) {
            error_ = "expected partition count";
            return std::nullopt;
        }
        int n = std::atoi(countStr->c_str());
        if (n <= 0) {
            error_ = "partition count must be positive";
            return std::nullopt;
        }
        ddl.schema.partition = PartitionPolicy{
            PartitionPolicy::Kind::Hash,
            *colName,
            static_cast<std::uint32_t>(n),
            {},
            {}
        };
    }

    return ddl;
}

bool SqlParser::parseColumnSpec(ColumnDef& out, bool& hasPrimaryKey) {
    auto name = consumeIdentifier();
    if (!name) {
        error_ = "expected column name";
        return false;
    }
    out.name = *name;

    // Type
    if (consumeKeyword("INT") || consumeKeyword("INTEGER")) {
        out.type = ValueType::Int64;
    } else if (consumeKeyword("BIGINT")) {
        out.type = ValueType::Int64;
    } else if (consumeKeyword("SMALLINT")) {
        out.type = ValueType::Int32;
    } else if (consumeKeyword("DOUBLE") || consumeKeyword("REAL") || consumeKeyword("FLOAT")) {
        out.type = ValueType::Double;
    } else if (consumeKeyword("TEXT") || consumeKeyword("VARCHAR") || consumeKeyword("CHAR")) {
        out.type = ValueType::Text;
    } else if (consumeKeyword("BLOB") || consumeKeyword("BYTEA")) {
        out.type = ValueType::Blob;
    } else if (consumeKeyword("DATE")) {
        out.type = ValueType::Date;
    } else if (consumeKeyword("TIMESTAMP")) {
        out.type = ValueType::Timestamp;
    } else if (consumeKeyword("BOOL") || consumeKeyword("BOOLEAN")) {
        out.type = ValueType::Bool;
    } else {
        error_ = "unknown column type";
        return false;
    }

    // Optional constraints
    while (true) {
        skipWhitespaceAndComments();
        if (consumeKeyword("PRIMARY")) {
            if (!consumeKeyword("KEY")) {
                error_ = "expected KEY after PRIMARY";
                return false;
            }
            if (hasPrimaryKey) {
                error_ = "only one PRIMARY KEY allowed";
                return false;
            }
            out.primaryKey = true;
            hasPrimaryKey = true;
            continue;
        }
        if (consumeKeyword("NOT")) {
            if (!consumeKeyword("NULL")) {
                error_ = "expected NULL after NOT";
                return false;
            }
            out.nullable = false;
            continue;
        }
        if (consumeKeyword("NULL")) {
            out.nullable = true;
            continue;
        }
        // ponytail: DEFAULT <literal> — Phase 5 Item 7
        // Supports int/float/string/bool/null literals. NULL default is
        // accepted; non-null literal on a NOT NULL column is the caller's
        // responsibility to validate (the storage provider can check).
        if (consumeKeyword("DEFAULT")) {
            if (consumeKeyword("NULL")) {
                out.defaultValue = Value::null();
            } else {
                auto dv = consumeValueLiteral();
                if (!dv) {
                    error_ = "expected literal after DEFAULT";
                    return false;
                }
                out.defaultValue = *dv;
            }
            continue;
        }
        break;
    }
    return true;
}

std::optional<DdlDropTable> SqlParser::parseDropTable() {
    auto tableName = consumeIdentifier();
    if (!tableName) {
        error_ = "expected table name after DROP TABLE";
        return std::nullopt;
    }
    return DdlDropTable{*tableName};
}

// -----------------------------------------------------------------------------
// DML parsing
// -----------------------------------------------------------------------------
std::optional<Command> SqlParser::parseInsert() {
    // Optional INTO
    consumeKeyword("INTO");
    auto table = consumeIdentifier();
    if (!table) {
        error_ = "expected table name after INSERT";
        return std::nullopt;
    }

    std::vector<std::string> columns;
    if (consumeSymbol("(")) {
        while (true) {
            auto col = consumeIdentifier();
            if (!col) { error_ = "expected column name"; return std::nullopt; }
            columns.push_back(*col);
            if (consumeSymbol(",")) continue;
            if (consumeSymbol(")")) break;
            error_ = "expected ',' or ')' in column list";
            return std::nullopt;
        }
    }

    if (!consumeKeyword("VALUES")) {
        // ponytail: INSERT INTO foo DEFAULT VALUES — Phase 5 Item 7.
        // Emits an empty Tuple plus a sentinel "all-defaults" marker. The
        // EngineLoop (or front-end) is responsible for materializing per-row
        // defaults from the schema. We mark it with a one-element Tuple
        // containing Value::null() and an empty columns list; the receiver
        // checks both signals.
        if (consumeKeyword("DEFAULT")) {
            if (!consumeKeyword("VALUES")) {
                error_ = "expected VALUES after DEFAULT";
                return std::nullopt;
            }
            Tuple t;
            // The empty Tuple + empty columns list is the "use defaults" signal.
            return Command(CommandType::Insert, *table, std::nullopt, std::move(t));
        }
        error_ = "expected VALUES or DEFAULT VALUES";
        return std::nullopt;
    }

    Tuple t = parseInsertValueList();
    // If columns were specified, we could reorder, but for now just trust order
    return Command(CommandType::Insert, *table, std::nullopt, std::move(t));
}

std::optional<Command> SqlParser::parseSelect() {
    std::vector<std::string> projections;
    // Optional projections
    if (consumeSymbol("*")) {
        projections.push_back("*");
    } else {
        while (true) {
            auto col = consumeIdentifier();
            if (!col) { error_ = "expected column name in SELECT"; return std::nullopt; }
            projections.push_back(*col);
            if (consumeSymbol(",")) continue;
            break;
        }
    }

    if (!consumeKeyword("FROM")) {
        error_ = "expected FROM after SELECT";
        return std::nullopt;
    }
    auto table = consumeIdentifier();
    if (!table) {
        error_ = "expected table name after FROM";
        return std::nullopt;
    }

    std::optional<Predicate> where;
    if (consumeKeyword("WHERE")) {
        auto pred = parsePredicate();
        if (!pred) return std::nullopt;
        where = Predicate(std::move(pred));
    }

    // ponytail: ORDER BY col [ASC|DESC] [, col ...]  → OrderBySpec list
    std::vector<OrderBySpec> orderBy;
    if (consumeKeyword("ORDER")) {
        if (!consumeKeyword("BY")) {
            error_ = "expected BY after ORDER";
            return std::nullopt;
        }
        while (true) {
            auto col = consumeIdentifier();
            if (!col) { error_ = "expected column name in ORDER BY"; return std::nullopt; }
            SortDirection dir = SortDirection::Asc;
            if (consumeKeyword("ASC")) {
                dir = SortDirection::Asc;
            } else if (consumeKeyword("DESC")) {
                dir = SortDirection::Desc;
            }
            orderBy.push_back({*col, dir});
            if (consumeSymbol(",")) continue;
            break;
        }
    }

    std::optional<std::size_t> limitVal, offsetVal;
    if (consumeKeyword("LIMIT")) {
        auto v = consumeValueLiteral();
        if (!v || (!v->isInt32() && !v->isInt64())) {
            error_ = "expected integer after LIMIT";
            return std::nullopt;
        }
        limitVal = v->isInt64() ? static_cast<std::size_t>(v->asInt64())
                                : static_cast<std::size_t>(v->asInt32());
        if (consumeKeyword("OFFSET")) {
            auto off = consumeValueLiteral();
            if (!off || (!off->isInt32() && !off->isInt64())) {
                error_ = "expected integer after OFFSET";
                return std::nullopt;
            }
            offsetVal = off->isInt64() ? static_cast<std::size_t>(off->asInt64())
                                       : static_cast<std::size_t>(off->asInt32());
        }
    } else if (consumeKeyword("OFFSET")) {
        auto off = consumeValueLiteral();
        if (!off || (!off->isInt32() && !off->isInt64())) {
            error_ = "expected integer after OFFSET";
            return std::nullopt;
        }
        offsetVal = off->isInt64() ? static_cast<std::size_t>(off->asInt64())
                                   : static_cast<std::size_t>(off->asInt32());
    }

    Command cmd(CommandType::Select, *table,
                std::move(where), std::nullopt,
                std::move(projections));
    if (!orderBy.empty()) cmd.withOrderBy(std::move(orderBy));
    if (limitVal)  cmd.withLimit(*limitVal);
    if (offsetVal) cmd.withOffset(*offsetVal);
    return cmd;
}

std::optional<Command> SqlParser::parseUpdate() {
    auto table = consumeIdentifier();
    if (!table) { error_ = "expected table name after UPDATE"; return std::nullopt; }

    if (!consumeKeyword("SET")) {
        error_ = "expected SET after table name";
        return std::nullopt;
    }

    Tuple values;
    int idx = 0;
    while (true) {
        auto col = consumeIdentifier();
        if (!col) { error_ = "expected column name in SET"; return std::nullopt; }
        if (!consumeSymbol("=")) { error_ = "expected '=' after column"; return std::nullopt; }
        auto val = consumeValueLiteral();
        if (!val) return std::nullopt;
        values.set(*col, *val);
        ++idx;

        skipWhitespaceAndComments();
        if (consumeSymbol(",")) continue;
        break;
    }

    std::optional<Predicate> where;
    if (consumeKeyword("WHERE")) {
        auto pred = parsePredicate();
        if (!pred) return std::nullopt;
        where = Predicate(std::move(pred));
    }

    return Command(CommandType::Update, *table, std::move(where), std::move(values));
}

std::optional<Command> SqlParser::parseDelete() {
    // Optional FROM
    consumeKeyword("FROM");
    auto table = consumeIdentifier();
    if (!table) { error_ = "expected table name after DELETE"; return std::nullopt; }

    std::optional<Predicate> where;
    if (consumeKeyword("WHERE")) {
        auto pred = parsePredicate();
        if (!pred) return std::nullopt;
        where = Predicate(std::move(pred));
    }

    return Command(CommandType::Delete, *table, std::move(where), std::nullopt);
}

// -----------------------------------------------------------------------------
// Predicate parsing (recursive descent: OR -> AND -> PRIMARY)
// -----------------------------------------------------------------------------
std::unique_ptr<PredicateNode> SqlParser::parsePredicate() {
    return parseOrExpr();
}

std::unique_ptr<PredicateNode> SqlParser::parseOrExpr() {
    auto left = parseAndExpr();
    while (true) {
        skipWhitespaceAndComments();
        if (!consumeKeyword("OR")) break;
        auto right = parseAndExpr();
        std::vector<std::unique_ptr<PredicateNode>> kids;
        kids.push_back(std::move(left));
        kids.push_back(std::move(right));
        left = LogicalNode::make(LogicalNode::Op::Or, std::move(kids));
    }
    return left;
}

std::unique_ptr<PredicateNode> SqlParser::parseAndExpr() {
    auto left = parsePrimExpr();
    while (true) {
        skipWhitespaceAndComments();
        if (!consumeKeyword("AND")) break;
        auto right = parsePrimExpr();
        std::vector<std::unique_ptr<PredicateNode>> kids;
        kids.push_back(std::move(left));
        kids.push_back(std::move(right));
        left = LogicalNode::make(LogicalNode::Op::And, std::move(kids));
    }
    return left;
}

std::unique_ptr<PredicateNode> SqlParser::parsePrimExpr() {
    skipWhitespaceAndComments();
    auto col = consumeIdentifier();
    if (!col) {
        error_ = "expected column name in predicate";
        return nullptr;
    }
    auto op = parseComparisonOp();
    if (!op) {
        error_ = "expected comparison operator";
        return nullptr;
    }
    auto val = consumeValueLiteral();
    if (!val) return nullptr;
    return ComparisonNode::make(*col, *op, *val);
}

std::optional<ComparisonOp> SqlParser::parseComparisonOp() {
    skipWhitespaceAndComments();
    if (consumeSymbol("=")) return ComparisonOp::Equals;
    if (consumeSymbol("!=")) return ComparisonOp::NotEquals;
    if (consumeSymbol("<=")) return ComparisonOp::LessEquals;
    if (consumeSymbol(">=")) return ComparisonOp::GreaterEquals;
    if (consumeSymbol("<")) return ComparisonOp::Less;
    if (consumeSymbol(">")) return ComparisonOp::Greater;
    return std::nullopt;
}

// -----------------------------------------------------------------------------
// INSERT value list parsing
// -----------------------------------------------------------------------------
Tuple SqlParser::parseInsertValueList() {
    Tuple t;
    if (!consumeSymbol("(")) {
        error_ = "expected '(' after VALUES";
        return t;
    }
    while (true) {
        auto val = consumeValueLiteral();
        if (!val) return t;
        // Anonymous column: use position as name
        std::string colName = std::to_string(t.size());
        t.set(colName, *val);
        if (consumeSymbol(",")) continue;
        if (consumeSymbol(")")) break;
        error_ = "expected ',' or ')' in value list";
        return t;
    }
    return t;
}

} // namespace atomdb