#include "test_framework.hpp"

#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/Value.hpp"

using namespace atomdb;

TEST(Tuple_Construct_InitializerList) {
    Tuple t = Tuple::make({{"age", Value::int32(30)},
                            {"name", Value::text("nikhil")}});
    EXPECT(t.has("age"));
    EXPECT(t.has("name"));
    EXPECT(!t.has("missing"));
    EXPECT_EQ(t.get("age").asInt32(), 30);
    EXPECT(t.get("name").asText() == "nikhil");
    EXPECT_THROWS(t.get("missing"));
}

TEST(Tuple_Set_Existing_Replaces_In_Place) {
    Tuple t = Tuple::make({{"a", Value::int32(1)}, {"b", Value::int32(2)}});
    t.set("a", Value::int32(99));
    EXPECT_EQ(t.get("a").asInt32(), 99);
    // Order is preserved: b should still be the second column.
    EXPECT_EQ(t.size(), std::size_t{2});
    const auto& cols = t.columns();
    EXPECT(cols[0].name == "a");
    EXPECT(cols[1].name == "b");
}

TEST(Tuple_Set_New_Column_Appends) {
    Tuple t = Tuple::make({{"a", Value::int32(1)}});
    t.set("b", Value::int32(2));
    EXPECT(t.has("b"));
    EXPECT_EQ(t.size(), std::size_t{2});
    const auto& cols = t.columns();
    EXPECT(cols[0].name == "a");
    EXPECT(cols[1].name == "b");
}

TEST(Tuple_MaybeGet_Returns_Nullopt_For_Missing) {
    Tuple t = Tuple::make({{"a", Value::int32(1)}});
    auto mv = t.maybeGet("missing");
    EXPECT(!mv.has_value());
    auto mp = t.maybeGet("a");
    EXPECT(mp.has_value());
    EXPECT_EQ(mp->asInt32(), 1);
}

TEST(Tuple_Order_Sensitive_Equality) {
    Tuple t1 = Tuple::make({{"a", Value::int32(1)}, {"b", Value::int32(2)}});
    Tuple t2 = Tuple::make({{"a", Value::int32(1)}, {"b", Value::int32(2)}});
    Tuple t3 = Tuple::make({{"b", Value::int32(2)}, {"a", Value::int32(1)}}); // swapped cols
    EXPECT(t1 == t2);
    EXPECT(!(t1 == t3)); // same values, different order -> not equal
}

TEST(Tuple_ToString_Round_Trip) {
    Tuple t = Tuple::make({{"name", Value::text("nikhil")}, {"age", Value::int32(30)}});
    const std::string s = t.toString();
    EXPECT(s == "name=nikhil,age=30");
}
