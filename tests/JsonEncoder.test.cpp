#include "opendb/frontend/JsonEncoder.hpp"
#include "opendb/types/Result.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/Value.hpp"
#include "opendb/types/DbError.hpp"
#include "test_framework.hpp"

using namespace opendb;

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

// ---------------------------------------------------------------------------
// JSON request parser tests (ponytail: F.6/I.8)
// ---------------------------------------------------------------------------

TEST(JsonParser_Query_Extracts_Type_And_Sql) {
    auto req = parseJsonRequest(R"({"type":"query","sql":"SELECT 1"})");
    EXPECT(req.has_value());
    if (req) {
        EXPECT(req->type == "query");
        EXPECT(req->sql == "SELECT 1");
    }
}

TEST(JsonParser_Begin_Has_Type_Only) {
    auto req = parseJsonRequest(R"({"type":"begin"})");
    EXPECT(req.has_value());
    if (req) EXPECT(req->type == "begin");
}

TEST(JsonParser_Commit_With_TxnId) {
    auto req = parseJsonRequest(R"({"type":"commit","txnId":42})");
    EXPECT(req.has_value());
    if (req) {
        EXPECT(req->type == "commit");
        EXPECT(req->txnId.has_value());
        EXPECT_EQ(*req->txnId, std::uint64_t{42});
    }
}

TEST(JsonParser_Escaped_Quotes_Inside_SQL) {
    auto req = parseJsonRequest(R"({"type":"query","sql":"SELECT \"foo\" FROM t"})");
    EXPECT(req.has_value());
    if (req) {
        EXPECT(req->type == "query");
        EXPECT(req->sql == "SELECT \"foo\" FROM t");
    }
}

TEST(JsonParser_SQL_Containing_Substring_sql) {
    auto req = parseJsonRequest(R"json({"type":"query","sql":"CREATE TABLE sql_log (id INT)"})json");
    EXPECT(req.has_value());
    if (req) {
        EXPECT(req->type == "query");
        EXPECT(req->sql == "CREATE TABLE sql_log (id INT)");
    }
}

TEST(JsonParser_String_With_Backslash_Escapes) {
    auto req = parseJsonRequest(R"({"type":"query","sql":"a\nb\tc\\d"})");
    EXPECT(req.has_value());
    if (req) EXPECT(req->sql == "a\nb\tc\\d");
}

TEST(JsonParser_Unknown_Key_Is_Ignored) {
    auto req = parseJsonRequest(R"({"type":"begin","extra":"ignored","nested":{"a":1}})");
    EXPECT(req.has_value());
    if (req) EXPECT(req->type == "begin");
}

TEST(JsonParser_Key_Order_Does_Not_Matter) {
    auto req = parseJsonRequest(R"({"sql":"SELECT 1","type":"query"})");
    EXPECT(req.has_value());
    if (req) {
        EXPECT(req->type == "query");
        EXPECT(req->sql == "SELECT 1");
    }
}

TEST(JsonParser_Whitespace_Tolerated) {
    auto req = parseJsonRequest(R"({ "type" : "query" , "sql" : "SELECT 1" })");
    EXPECT(req.has_value());
    if (req) {
        EXPECT(req->type == "query");
        EXPECT(req->sql == "SELECT 1");
    }
}

TEST(JsonParser_Malformed_Returns_Nullopt) {
    EXPECT(!parseJsonRequest("{not valid json}").has_value());
    EXPECT(!parseJsonRequest(R"({"type":})").has_value());
    EXPECT(!parseJsonRequest(R"({"sql":"x"})").has_value());
    EXPECT(!parseJsonRequest(R"({"type":"begin")").has_value());
    EXPECT(!parseJsonRequest("").has_value());
    EXPECT(!parseJsonRequest("[]").has_value());
}

TEST(JsonParser_Unicode_Escape_In_SQL) {
    auto req = parseJsonRequest("{\"type\":\"query\",\"sql\":\"caf\\u00e9\"}");
    EXPECT(req.has_value());
    if (req) {
        EXPECT(req->sql == std::string("caf\xc3\xa9"));
    }
}

TEST(JsonParser_TxnId_As_Number_String) {
    auto req = parseJsonRequest(R"({"type":"commit","txnId":9999999999})");
    EXPECT(req.has_value());
    if (req) {
        EXPECT(req->txnId.has_value());
        EXPECT_EQ(*req->txnId, std::uint64_t{9999999999ULL});
    }
}
