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

// ---- Phase 4: extended tags (Blob, Date, Timestamp) + binary wire format ----

TEST(Value_Construct_Blob) {
    auto v = Value::blob({0x01, 0x02, 0x03, 0xFF});
    EXPECT(v.isBlob());
    EXPECT(!v.isText());
    EXPECT_EQ(v.asBlob().size(), 4u);
    EXPECT_EQ(v.asBlob()[0], 0x01);
    EXPECT_EQ(v.asBlob()[3], 0xFF);
}

TEST(Value_Construct_Date) {
    auto v = Value::date(0);  // 1970-01-01
    EXPECT(v.isDate());
    EXPECT(!v.isInt32());  // distinct tag, even though stored in the same slot
    EXPECT_EQ(v.asDate(), 0);
    // ISO 8601 string for epoch.
    EXPECT(v.toString() == "1970-01-01");
}

TEST(Value_Construct_Timestamp) {
    auto v = Value::timestamp(0);  // 1970-01-01T00:00:00.000Z
    EXPECT(v.isTimestamp());
    EXPECT(!v.isInt64());
    EXPECT_EQ(v.asTimestamp(), 0);
    EXPECT(v.toString() == "1970-01-01T00:00:00.000Z");
}

TEST(Value_Ordering_Blob_After_Text) {
    // Blob lives in a higher bucket than Text.
    EXPECT(Value::text("zzz") < Value::blob({0x00}));
    // Within Blob, lexicographic byte comparison.
    EXPECT(Value::blob({0x01, 0x02}) < Value::blob({0x01, 0x03}));
    EXPECT(Value::blob({0x01}) < Value::blob({0x01, 0x00}));
}

TEST(Value_Ordering_Date_Timestamp_In_Number_Bucket) {
    // Date and Timestamp share the Number bucket with Int32/Int64/Double.
    // They compare numerically against each other.
    EXPECT(Value::date(0).compare(Value::int32(0)) == std::strong_ordering::equal);
    EXPECT(Value::timestamp(0).compare(Value::int64(0)) == std::strong_ordering::equal);
    // Date vs Text: Date is in Number, Text is later bucket.
    EXPECT(Value::date(1000) < Value::text(""));
    // Tag-true equality still requires same tag.
    EXPECT(!(Value::date(0) == Value::int32(0)));
    EXPECT(!(Value::timestamp(0) == Value::int64(0)));
}

TEST(Value_Binary_Round_Trip_All_Tags) {
    // For every ValueType, write toBytes() then parse back via fromBytes()
    // and confirm the result equals the original (tag-true equality).
    Value originals[] = {
        Value::null(),
        Value::boolean(true),
        Value::boolean(false),
        Value::int32(-42),
        Value::int64(0x123456789ABCDEF0LL),
        Value::real(3.14159),
        Value::text("hello world"),
        Value::blob({0xFF, 0x00, 0x7F, 0x80, 0x01}),
        Value::date(19723),  // 2024-01-01
        Value::timestamp(1704067200000LL),  // 2024-01-01T00:00:00.000Z
    };
    for (const auto& orig : originals) {
        auto bytes = orig.toBytes();
        std::vector<std::uint8_t>::const_iterator it = bytes.cbegin();
        Value parsed = Value::fromBytes(it);
        EXPECT(parsed == orig);
        EXPECT(it == bytes.cend());  // all bytes consumed
    }
}

TEST(Value_Binary_Empty_Text_And_Blob) {
    // Zero-length Text and Blob must round-trip (and not misread as Null).
    auto t = Value::text("");
    auto tb = t.toBytes();
    std::vector<std::uint8_t>::const_iterator it = tb.cbegin();
    EXPECT(Value::fromBytes(it) == t);
    EXPECT(it == tb.cend());

    auto b = Value::blob({});
    auto bb = b.toBytes();
    std::vector<std::uint8_t>::const_iterator it2 = bb.cbegin();
    EXPECT(Value::fromBytes(it2) == b);
    EXPECT(it2 == bb.cend());
}
