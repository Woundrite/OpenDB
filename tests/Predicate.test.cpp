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
