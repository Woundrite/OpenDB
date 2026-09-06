#ifndef OPENDB_PREDICATE_HPP
#define OPENDB_PREDICATE_HPP

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "opendb/types/Tuple.hpp"
#include "opendb/types/Value.hpp"

namespace opendb {

// Predicate tree (spec §3.3) — recursive WHERE expression tree.
//   - ComparisonNode: leaves which compare a column to a constant operand.
//   - LogicalNode: internal nodes combining children with AND/OR.
// AND short-circuits to false on the first false child; OR short-circuits to
// true on the first true child (spec §3.3, verified with a CountingNode in tests).

enum class ComparisonOp {
    Equals,
    NotEquals,
    Less,
    LessEquals,
    Greater,
    GreaterEquals,
    IsNull,
    IsNotNull,
};

class PredicateNode {
public:
    virtual ~PredicateNode() = default;
    virtual std::unique_ptr<PredicateNode> clone() const = 0;
    virtual bool evaluate(const Tuple& row) const = 0;
    virtual std::string toString() const = 0;
};

class ComparisonNode : public PredicateNode {
public:
    ComparisonNode(std::string column, ComparisonOp op, Value operand)
        : column_(std::move(column)), op_(op), operand_(std::move(operand)) {}

    static std::unique_ptr<ComparisonNode> make(std::string column,
                                                ComparisonOp op,
                                                Value operand) {
        return std::make_unique<ComparisonNode>(std::move(column), op,
                                                 std::move(operand));
    }

    std::unique_ptr<PredicateNode> clone() const override {
        return std::make_unique<ComparisonNode>(*this);
    }

    bool evaluate(const Tuple& row) const override {
        auto opt = row.maybeGet(column_);
        if (!opt) {
            // Column missing from row - treat as NULL for IS NULL / IS NOT NULL
            switch (op_) {
                case ComparisonOp::IsNull:     return true;
                case ComparisonOp::IsNotNull:  return false;
                default:                       return false;
            }
        }
        const auto& rowVal = *opt;
        const bool isNull = rowVal.isNull();
        const bool operandIsNull = operand_.isNull();
        
        switch (op_) {
            case ComparisonOp::Equals:        return !isNull && !operandIsNull && rowVal.compare(operand_) == 0;
            case ComparisonOp::NotEquals:     return !isNull && !operandIsNull && rowVal.compare(operand_) != 0;
            case ComparisonOp::Less:          return !isNull && !operandIsNull && rowVal.compare(operand_) <  0;
            case ComparisonOp::LessEquals:    return !isNull && !operandIsNull && rowVal.compare(operand_) <= 0;
            case ComparisonOp::Greater:       return !isNull && !operandIsNull && rowVal.compare(operand_) >  0;
            case ComparisonOp::GreaterEquals: return !isNull && !operandIsNull && rowVal.compare(operand_) >= 0;
            case ComparisonOp::IsNull:        return isNull;
            case ComparisonOp::IsNotNull:     return !isNull;
        }
        return false; // unreachable
    }

    std::string toString() const override;

    const std::string& column() const noexcept { return column_; }
    ComparisonOp op() const noexcept { return op_; }
    const Value& operand() const noexcept { return operand_; }

private:
    std::string column_;
    ComparisonOp op_;
    Value operand_;
};

class LogicalNode : public PredicateNode {
public:
    enum class Op { And, Or };

    LogicalNode(Op op, std::vector<std::unique_ptr<PredicateNode>> children)
        : op_(op), children_(std::move(children)) {}

    static std::unique_ptr<LogicalNode> make(
        Op op, std::vector<std::unique_ptr<PredicateNode>> children) {
        return std::make_unique<LogicalNode>(op, std::move(children));
    }

    std::unique_ptr<PredicateNode> clone() const override {
        std::vector<std::unique_ptr<PredicateNode>> cloned;
        cloned.reserve(children_.size());
        for (const auto& c : children_) cloned.push_back(c->clone());
        return std::make_unique<LogicalNode>(op_, std::move(cloned));
    }

    bool evaluate(const Tuple& row) const override {
        if (op_ == Op::And) {
            for (const auto& c : children_) {
                if (!c->evaluate(row)) return false; // short-circuit on first false
            }
            return true;
        }
        // Op::Or
        for (const auto& c : children_) {
            if (c->evaluate(row)) return true; // short-circuit on first true
        }
        return false;
    }

    std::string toString() const override;

    Op op() const noexcept { return op_; }
    const std::vector<std::unique_ptr<PredicateNode>>& children() const noexcept {
        return children_;
    }

private:
    Op op_;
    std::vector<std::unique_ptr<PredicateNode>> children_;
};

// Predicate: owner wrapper, the type carried around by Command. Move-only-ish:
// copyable via clone() so it can live inside std::optional<Predicate>.
class Predicate {
public:
    Predicate() = default;
    explicit Predicate(std::unique_ptr<PredicateNode> root)
        : root_(std::move(root)) {}

    Predicate(const Predicate& other) {
        if (other.root_) root_ = other.root_->clone();
    }
    Predicate& operator=(const Predicate& other) {
        if (this != &other) {
            root_.reset();
            if (other.root_) root_ = other.root_->clone();
        }
        return *this;
    }
    Predicate(Predicate&&) noexcept = default;
    Predicate& operator=(Predicate&&) noexcept = default;

    bool evaluate(const Tuple& row) const {
        return root_ ? root_->evaluate(row) : false;
    }

    bool empty() const noexcept { return !root_; }

    std::string toString() const {
        return root_ ? root_->toString() : std::string{};
    }

    // Worked example from spec §3.3: WHERE age > 25 AND name = 'nikhil'.
    // Used by tests; convenient leaf factory for ad-hoc construction elsewhere.
    static Predicate example() {
        auto left = ComparisonNode::make("age", ComparisonOp::Greater,
                                           Value::int32(25));
        auto right = ComparisonNode::make("name", ComparisonOp::Equals,
                                            Value::text("nikhil"));
        std::vector<std::unique_ptr<PredicateNode>> kids;
        kids.push_back(std::move(left));
        kids.push_back(std::move(right));
        return Predicate(LogicalNode::make(LogicalNode::Op::And,
                                            std::move(kids)));
    }

    const PredicateNode* root() const noexcept { return root_.get(); }

private:
    std::unique_ptr<PredicateNode> root_;
};

// --- toString definitions (defined out-of-line for clarity) ----------------

inline std::string ComparisonNode::toString() const {
    const char* op_str = "?";
    switch (op_) {
        case ComparisonOp::Equals:        op_str = "=";       break;
        case ComparisonOp::NotEquals:     op_str = "!=";      break;
        case ComparisonOp::Less:          op_str = "<";       break;
        case ComparisonOp::LessEquals:    op_str = "<=";      break;
        case ComparisonOp::Greater:       op_str = ">";       break;
        case ComparisonOp::GreaterEquals: op_str = ">=";      break;
        case ComparisonOp::IsNull:        op_str = "IS NULL"; break;
        case ComparisonOp::IsNotNull:     op_str = "IS NOT NULL"; break;
    }
    // For IS NULL / IS NOT NULL, the operand is not printed
    if (op_ == ComparisonOp::IsNull || op_ == ComparisonOp::IsNotNull) {
        return "(" + column_ + " " + op_str + ")";
    }
    return "(" + column_ + " " + op_str + " " + operand_.toString() + ")";
}

inline std::string LogicalNode::toString() const {
    const char* op_str = (op_ == Op::And) ? "AND" : "OR";
    std::string s = "(";
    for (std::size_t i = 0; i < children_.size(); ++i) {
        if (i) {
            s += " ";
            s += op_str;
            s += " ";
        }
        s += children_[i] ? children_[i]->toString() : "<null>";
    }
    s += ")";
    return s;
}

} // namespace opendb

#endif // OPENDB_PREDICATE_HPP
