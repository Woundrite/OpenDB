#include "atomdb/frontend/JsonEncoder.hpp"
#include "atomdb/types/Result.hpp"
#include "atomdb/types/Tuple.hpp"
#include "atomdb/types/Value.hpp"
#include "atomdb/types/DbError.hpp"
#include "test_framework.hpp"

using namespace atomdb;

TEST(JsonEncoder_Null_Bool_Numbers_Roundtrip_Literals) {
    ResultSet rs;
    rs.success = true;
    rs.rows.push_back(Tuple::make({
        {"n",  Value::null()},
        {"b",  Value::boolean(true)},
        {"i32", Value::int32(-7)},
        {"i64", Value::int64(123)},
        {"d",  Value::real(2.5)},
    }));
    auto s = JsonEncoder::encode(rs);
    EXPECT(s.find("\"n\":null") != std::string::npos);
    EXPECT(s.find("\"b\":true") != std::string::npos);
    EXPECT(s.find("\"i32\":-7") != std::string::npos);
    EXPECT(s.find("\"i64\":123") != std::string::npos);
}

TEST(JsonEncoder_Blob_Encoded_As_Base64) {
    auto s = JsonEncoder::encode(Value::blob({0x00, 0x01, 0x02, 0x03}));
    EXPECT(s == "\"AAECAw==\"");
}

TEST(JsonEncoder_Date_Formatted_As_ISO8601) {
    EXPECT(JsonEncoder::encode(Value::date(0)) == "\"1970-01-01\"");
    EXPECT(JsonEncoder::encode(Value::date(1)) == "\"1970-01-02\"");
    // 2024-01-01 is day 19723 days after 1970-01-01
    EXPECT(JsonEncoder::encode(Value::date(19723)) == "\"2024-01-01\"");
}

TEST(JsonEncoder_Timestamp_Formatted_As_ISO8601_With_Ms) {
    EXPECT(JsonEncoder::encode(Value::timestamp(0)) == "\"1970-01-01T00:00:00.000Z\"");
    // 1735689600000 ms = 2025-01-01T00:00:00.000Z
    EXPECT(JsonEncoder::encode(Value::timestamp(1735689600000LL)) == "\"2025-01-01T00:00:00.000Z\"");
}

TEST(JsonEncoder_Escapes_Standard_Control_Chars) {
    auto s = JsonEncoder::encode(ResultSet(true, {Tuple::make({
        {"txt", Value::text(std::string("line1\nline2\t\"q\""))}
    })}));
    EXPECT(s.find("\\n") != std::string::npos);
    EXPECT(s.find("\\t") != std::string::npos);
    EXPECT(s.find("\\\"q\\\"") != std::string::npos);
}

TEST(JsonEncoder_Error_Encodes_Success_False) {
    auto err = DbError::internal("boom");
    auto s = JsonEncoder::encode(err);
    EXPECT(s.find("\"success\":false") != std::string::npos);
    EXPECT(s.find("boom") != std::string::npos);
}

TEST(JsonEncoder_ArrayOfArrays_Compact_Format) {
    ResultSet rs;
    rs.success = true;
    rs.rows.push_back(Tuple::make({
        {"id", Value::int64(1)},
        {"name", Value::text("alice")},
    }));
    rs.rows.push_back(Tuple::make({
        {"id", Value::int64(2)},
        {"name", Value::text("bob")},
    }));
    auto s = JsonEncoder::encodeArray(rs);
    // No quoted column names; values sit in arrays.
    EXPECT(s.find("\"id\":") == std::string::npos);
    EXPECT(s.find("[1,\"alice\"]") != std::string::npos);
    EXPECT(s.find("[2,\"bob\"]") != std::string::npos);
}
