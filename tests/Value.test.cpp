#include "test_framework.hpp"

#include "atomdb/types/Value.hpp"

using namespace atomdb;

// Tag order per spec.
// Test document comment: Int32(5) and Int64(5) compare numerically equal
// (shared Number bucket), but operator== returns false because the tags differ.

TEST(Value_Construct_With_Each_Tag) {
    EXPECT(Value::null().isNull());
    EXPECT(Value::boolean(true).isBool());
    EXPECT(Value::int32(7).isInt32());
    EXPECT(Value::int64(7ll).isInt64());
    EXPECT(Value::real(1.5).isDouble());
    EXPECT(Value::text("hi").isText());

    EXPECT_EQ(Value::boolean(true).asBool(), true);
    EXPECT_EQ(Value::int32(7).asInt32(), 7);
    EXPECT_EQ(Value::int64(7ll).asInt64(), 7);
    EXPECT(Value::real(1.5).asDouble() == 1.5);
    EXPECT(Value::text("hi").asText() == "hi");
}

TEST(Value_ToString_Per_Tag) {
    EXPECT(Value::null().toString() == "NULL");
    EXPECT(Value::boolean(true).toString() == "true");
    EXPECT(Value::boolean(false).toString() == "false");
    EXPECT(Value::int32(42).toString() == "42");
    EXPECT(Value::int64(99ll).toString() == "99");
    EXPECT(Value::real(3.5).toString() == "3.5");
    // integer-valued double should still show ".5" or ".0"-trimmed representation.
    EXPECT(Value::real(3.0).toString() == "3");
    EXPECT(Value::text("hello").toString() == "hello");
}

TEST(Value_Ordering_Across_Tags) {
    // Null < Bool(false) < Bool(true) < Number(0) < Number(100) < Text("a") < Text("b")
    EXPECT(Value::null()           < Value::boolean(false));
    EXPECT(Value::boolean(false)   < Value::boolean(true));
    EXPECT(Value::boolean(true)    < Value::int32(0));
    EXPECT(Value::int32(0)         < Value::int32(100));
    EXPECT(Value::int32(100)       < Value::text("a"));
    EXPECT(Value::text("a")        < Value::text("b"));
    // Cross-numeric-tag ordering within Number bucket.
    EXPECT(Value::int32(5)         < Value::int64(100));
    EXPECT(Value::int64(-3)        < Value::int32(0));
    EXPECT(Value::real(0.5)        < Value::int32(1));
}

TEST(Value_Ordering_Numeric_Compare_Equal_Across_Tags) {
    // compare() says they are numerically equal, but operator== says they aren't
    // because the tags differ. Document this divergence in the test.
    const Value a = Value::int32(5);
    const Value b = Value::int64(5);
    EXPECT(a.compare(b) == std::strong_ordering::equal);
    EXPECT(!(a == b));
    // but Double exactly equal to int compares numerically equal too.
    const Value c = Value::real(5.0);
    EXPECT(a.compare(c) == std::strong_ordering::equal);
    EXPECT(c.compare(b) == std::strong_ordering::equal);
}

TEST(Value_Tag_True_Equality) {
    // Bool(true) != Int64(1) even though both are non-zero.
    EXPECT(!(Value::boolean(true) == Value::int64(1)));
    // Null == Null.
    EXPECT(Value::null() == Value::null());
    // Int32 vs Int64 differ by tag.
    EXPECT(!(Value::int32(1) == Value::int64(1)));
    // Same-tag same-value are equal.
    EXPECT(Value::int32(7) == Value::int32(7));
    EXPECT(Value::text("a") == Value::text("a"));
}
