#include "test_framework.hpp"

#include <memory>
#include <vector>

#include "atomdb/types/Predicate.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/Value.hpp"

using namespace atomdb;

// --- helpers used in the short-circuit test ---------------------------------
// A small node that records whether evaluate() was called so we can assert
// short-circuit behaviour in the spec §3.3 worked example AND/OR.
class CountingNode : public PredicateNode {
public:
    explicit CountingNode(bool result) : result_(result) {}
    std::unique_ptr<PredicateNode> clone() const override {
        return std::make_unique<CountingNode>(result_);
    }
    bool evaluate(const Tuple&) const override {
        ++times_;
        return result_;
    }
    std::string toString() const override { return "<counting>"; }
    // Counts are mutable but thread-unfriendly; this is a single-threaded test.
    mutable int times_ = 0;
    bool result_;
};

TEST(Predicate_Spec_Example_Evaluates_True) {
    const Predicate p = Predicate::example();
    Tuple happy = Tuple::make({{"age", Value::int32(30)},
                                {"name", Value::text("nikhil")}});
    EXPECT(p.evaluate(happy));
}

TEST(Predicate_Spec_Example_Evaluates_False_For_Failing_Left) {
    const Predicate p = Predicate::example();
    Tuple sad = Tuple::make({{"age", Value::int32(20)},  // 20 > 25 is false
                              {"name", Value::text("nikhil")}});
    EXPECT(!p.evaluate(sad));
}

TEST(Predicate_Spec_Example_Evaluates_False_For_Failing_Right) {
    const Predicate p = Predicate::example();
    Tuple sad2 = Tuple::make({{"age", Value::int32(30)},
                               {"name", Value::text("someone-else")}});
    EXPECT(!p.evaluate(sad2));
}

TEST(Predicate_And_Short_Circuits_On_First_False) {
    auto left  = std::make_unique<CountingNode>(false);
    auto right = std::make_unique<CountingNode>(true); // would-be evaluated if no short-circuit
    CountingNode* right_ptr = right.get();
    std::vector<std::unique_ptr<PredicateNode>> kids;
    kids.push_back(std::move(left));
    kids.push_back(std::move(right));
    Predicate p(LogicalNode::make(LogicalNode::Op::And, std::move(kids)));
    const Tuple empty;
    EXPECT(!p.evaluate(empty));
    EXPECT(right_ptr->times_ == 0); // right must NOT have been evaluated
}

TEST(Predicate_Or_Short_Circuits_On_First_True) {
    auto left  = std::make_unique<CountingNode>(true);
    auto right = std::make_unique<CountingNode>(true);
    CountingNode* right_ptr = right.get();
    std::vector<std::unique_ptr<PredicateNode>> kids;
    kids.push_back(std::move(left));
    kids.push_back(std::move(right));
    Predicate p(LogicalNode::make(LogicalNode::Op::Or, std::move(kids)));
    const Tuple empty;
    EXPECT(p.evaluate(empty));
    EXPECT(right_ptr->times_ == 0);
}

TEST(Predicate_And_No_Short_Circuit_Evaluates_All) {
    auto left  = std::make_unique<CountingNode>(true);
    auto right = std::make_unique<CountingNode>(true);
    CountingNode* left_ptr = left.get();
    CountingNode* right_ptr = right.get();
    std::vector<std::unique_ptr<PredicateNode>> kids;
    kids.push_back(std::move(left));
    kids.push_back(std::move(right));
    Predicate p(LogicalNode::make(LogicalNode::Op::And, std::move(kids)));
    const Tuple empty;
    EXPECT(p.evaluate(empty));
    EXPECT(left_ptr->times_  == 1);
    EXPECT(right_ptr->times_ == 1);
}

TEST(Predicate_Missing_Column_Returns_False) {
    Predicate p(ComparisonNode::make("missing", ComparisonOp::Equals,
                                       Value::int32(1)));
    const Tuple empty;
    EXPECT(!p.evaluate(empty));
}

TEST(Predicate_Clone_Yields_Independent_Subtree) {
    Predicate original = Predicate::example();
    Predicate copy = original; // copy-ctor calls clone()
    EXPECT(original.evaluate(Tuple::make({{"age", Value::int32(30)},
                                            {"name", Value::text("nikhil")}})));
    EXPECT(copy.evaluate(Tuple::make({{"age", Value::int32(30)},
                                        {"name", Value::text("nikhil")}})));
    // Mutating the copy's root pointer must not affect original.
    EXPECT(!copy.evaluate(Tuple::make({{"age", Value::int32(20)},
                                         {"name", Value::text("nikhil")}})));
    EXPECT(!original.evaluate(Tuple::make({{"age", Value::int32(20)},
                                             {"name", Value::text("nikhil")}})));
}

// ---------------------------------------------------------------------------
// Phase 6.4 / F.6: IS NULL / IS NOT NULL semantics.
//   - IS NULL matches missing columns and explicit NULLs; never matches a
//     non-null value.
//   - IS NOT NULL matches non-null values; never matches a missing column or
//     a NULL.
//   - x = NULL returns false (UNKNOWN coerced to false in our boolean
//     predicate tree; full three-valued logic is out of scope for spec §3.3).
// ---------------------------------------------------------------------------

TEST(Predicate_IsNull_Matches_Missing_Column) {
    Predicate p(ComparisonNode::make("col", ComparisonOp::IsNull, Value::null()));
    const Tuple empty;
    EXPECT(p.evaluate(empty));
}

TEST(Predicate_IsNull_Matches_Null_Value) {
    Predicate p(ComparisonNode::make("col", ComparisonOp::IsNull, Value::null()));
    Tuple row = Tuple::make({{"col", Value::null()}});
    EXPECT(p.evaluate(row));
}

TEST(Predicate_IsNull_Rejects_NonNull_Value) {
    Predicate p(ComparisonNode::make("col", ComparisonOp::IsNull, Value::null()));
    Tuple row = Tuple::make({{"col", Value::int32(42)}});
    EXPECT(!p.evaluate(row));
}

TEST(Predicate_IsNotNull_Matches_NonNull_Value) {
    Predicate p(ComparisonNode::make("col", ComparisonOp::IsNotNull, Value::null()));
    Tuple row = Tuple::make({{"col", Value::text("hello")}});
    EXPECT(p.evaluate(row));
}

TEST(Predicate_IsNotNull_Rejects_Missing_Column) {
    Predicate p(ComparisonNode::make("col", ComparisonOp::IsNotNull, Value::null()));
    const Tuple empty;
    EXPECT(!p.evaluate(empty));
}

TEST(Predicate_IsNotNull_Rejects_Null_Value) {
    Predicate p(ComparisonNode::make("col", ComparisonOp::IsNotNull, Value::null()));
    Tuple row = Tuple::make({{"col", Value::null()}});
    EXPECT(!p.evaluate(row));
}

TEST(Predicate_Equals_Null_Returns_False) {
    // WHERE x = NULL must not match (spec §3.3 three-valued logic: UNKNOWN).
    // The predicate tree is boolean-valued; UNKNOWN collapses to false.
    Predicate p(ComparisonNode::make("col", ComparisonOp::Equals, Value::null()));
    Tuple row = Tuple::make({{"col", Value::null()}});
    EXPECT(!p.evaluate(row));
    const Tuple empty;
    EXPECT(!p.evaluate(empty));
}

TEST(Predicate_NotEquals_Null_Returns_False) {
    // WHERE x != NULL similarly collapses to false (not UNKNOWN -> true).
    Predicate p(ComparisonNode::make("col", ComparisonOp::NotEquals, Value::null()));
    Tuple row = Tuple::make({{"col", Value::null()}});
    EXPECT(!p.evaluate(row));
}

TEST(Predicate_RegularOps_With_Null_Operand_Return_False) {
    // Regular comparisons (Equals, NotEquals, Less, etc.) with a NULL operand
    // must return false for any non-null row value (UNKNOWN → false).
    // Test NotEquals (the most critical: was buggy before fix).
    Predicate p1(ComparisonNode::make("col", ComparisonOp::NotEquals, Value::null()));
    Tuple row = Tuple::make({{"col", Value::int32(42)}});
    EXPECT(!p1.evaluate(row));
    
    // Test Greater (was buggy).
    Predicate p2(ComparisonNode::make("col", ComparisonOp::Greater, Value::null()));
    EXPECT(!p2.evaluate(row));
    
    // Test GreaterEquals (was buggy).
    Predicate p3(ComparisonNode::make("col", ComparisonOp::GreaterEquals, Value::null()));
    EXPECT(!p3.evaluate(row));
    
    // Test Equals.
    Predicate p4(ComparisonNode::make("col", ComparisonOp::Equals, Value::null()));
    EXPECT(!p4.evaluate(row));
    
    // Test Less.
    Predicate p5(ComparisonNode::make("col", ComparisonOp::Less, Value::null()));
    EXPECT(!p5.evaluate(row));
    
    // Test LessEquals.
    Predicate p6(ComparisonNode::make("col", ComparisonOp::LessEquals, Value::null()));
    EXPECT(!p6.evaluate(row));
}

TEST(Predicate_IsNull_ToString_Formats_Correctly) {
    Predicate p(ComparisonNode::make("col", ComparisonOp::IsNull, Value::null()));
    auto s = p.toString();
    EXPECT(s.find("IS NULL") != std::string::npos);
}

TEST(Predicate_IsNotNull_ToString_Formats_Correctly) {
    Predicate p(ComparisonNode::make("col", ComparisonOp::IsNotNull, Value::null()));
    auto s = p.toString();
    EXPECT(s.find("IS NOT NULL") != std::string::npos);
}
