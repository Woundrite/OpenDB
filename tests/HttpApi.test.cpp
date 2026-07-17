#include "atomdb/frontend/HttpApi.hpp"
#include "atomdb/storage/InMemoryStorageProvider.hpp"
#include "atomdb/types/Schema.hpp"
#include "atomdb/types/Value.hpp"
#include "atomdb/types/Result.hpp"
#include "atomdb/frontend/JsonEncoder.hpp"
#include "atomdb/contracts/IAccessPlugin.hpp"
#include "../tests/test_framework.hpp"
#include <memory>

// Simple mock dispatcher for testing
class MockDispatcher : public atomdb::IEngineDispatcher {
public:
    void enqueue(std::unique_ptr<atomdb::ISession> /*session*/) override {
        // Do nothing in test
    }
};

using namespace atomdb;

TEST(HttpApi_BasicQuery) {
    auto storage = std::make_unique<InMemoryStorageProvider>();
    storage->open("in-memory://");
    
    Schema schema;
    schema.table = "t";
    schema.columns = {
        ColumnDef{"id", ValueType::Int64, false, true, 0, {}},
        ColumnDef{"name", ValueType::Text, true, false, 0, {}},
    };
    EXPECT(storage->createTable(schema).isSentinel());
    
    MockDispatcher dispatcher;
    HttpApiAccessPlugin plugin;
    plugin.open("in-memory://", storage.get(), &dispatcher);
    
    // Create table via HTTP
    std::string req = "{\"type\":\"query\",\"sql\":\"CREATE TABLE t2 (id INT PRIMARY KEY, name TEXT)\"}";
    std::string resp = plugin.handleRequest(req);
    std::cout << "CREATE response: " << resp << std::endl;
    EXPECT(resp.find("\"success\":true") != std::string::npos);
    
    // Insert via HTTP
    req = "{\"type\":\"query\",\"sql\":\"INSERT INTO t2 VALUES (1, 'alice')\"}";
    resp = plugin.handleRequest(req);
    std::cout << "INSERT response: " << resp << std::endl;
    EXPECT(resp.find("\"success\":true") != std::string::npos);
    
    // Select via HTTP
    req = "{\"type\":\"query\",\"sql\":\"SELECT * FROM t2\"}";
    resp = plugin.handleRequest(req);
    std::cout << "SELECT response: " << resp << std::endl;
    EXPECT(resp.find("alice") != std::string::npos);
    EXPECT(resp.find("\"success\":true") != std::string::npos);
}

TEST(HttpApi_Insert_Select) {
    auto storage = std::make_unique<InMemoryStorageProvider>();
    storage->open("in-memory://");
    
    Schema schema;
    schema.table = "items";
    schema.columns = {
        ColumnDef{"id", ValueType::Int64, false, true, 0, {}},
        ColumnDef{"val", ValueType::Int64, true, false, 0, {}},
    };
    EXPECT(storage->createTable(schema).isSentinel());
    
    MockDispatcher dispatcher;
    HttpApiAccessPlugin plugin;
    plugin.open("in-memory://", storage.get(), &dispatcher);
    
    // Insert via HTTP
    std::string req = "{\"type\":\"query\",\"sql\":\"INSERT INTO items VALUES (1, 10)\"}";
    std::string resp = plugin.handleRequest(req);
    EXPECT(resp.find("\"success\":true") != std::string::npos);
    
    req = "{\"type\":\"query\",\"sql\":\"INSERT INTO items VALUES (2, 20)\"}";
    resp = plugin.handleRequest(req);
    EXPECT(resp.find("\"success\":true") != std::string::npos);
    
    // Select all
    req = "{\"type\":\"query\",\"sql\":\"SELECT * FROM items\"}";
    resp = plugin.handleRequest(req);
    EXPECT(resp.find("10") != std::string::npos);
    EXPECT(resp.find("20") != std::string::npos);
    EXPECT(resp.find("\"success\":true") != std::string::npos);
}

TEST(HttpApi_DropTable) {
    auto storage = std::make_unique<InMemoryStorageProvider>();
    storage->open("in-memory://");
    
    Schema schema;
    schema.table = "to_drop";
    schema.columns = {ColumnDef{"id", ValueType::Int64, false, true, 0, {}}};
    EXPECT(storage->createTable(schema).isSentinel());
    EXPECT(storage->tables().size() == 1);
    
    MockDispatcher dispatcher;
    HttpApiAccessPlugin plugin;
    plugin.open("in-memory://", storage.get(), &dispatcher);
    
    std::string resp = plugin.handleRequest("{\"type\":\"query\",\"sql\":\"DROP TABLE to_drop\"}");
    EXPECT(resp.find("\"success\":true") != std::string::npos);
    EXPECT(storage->tables().empty());
}

TEST(HttpApi_JsonEncoder) {
    ResultSet rs;
    rs.success = true;
    rs.rows = {
        Tuple::make({ColumnValue{"id", Value::int64(1)}, ColumnValue{"name", Value::text("test")}}),
        Tuple::make({ColumnValue{"id", Value::int64(2)}, ColumnValue{"name", Value::text("test2")}})
    };
    
    std::string json = JsonEncoder::encode(rs);
    EXPECT(json.find("\"success\":true") != std::string::npos);
    EXPECT(json.find("\"id\":1") != std::string::npos);
    EXPECT(json.find("\"name\":\"test\"") != std::string::npos);
    EXPECT(json.find("\"id\":2") != std::string::npos);
    EXPECT(json.find("\"name\":\"test2\"") != std::string::npos);
    
    // Error encoding
    DbError err = DbError::notFound("missing row");
    json = JsonEncoder::encode(err);
    EXPECT(json.find("missing row") != std::string::npos);
}

TEST(HttpApi_ParseJsonRequest) {
    auto req = parseJsonRequest("{\"type\":\"query\",\"sql\":\"SELECT 1\"}");
    EXPECT(req.has_value());
    if (req) {
        EXPECT(req->type == "query");
        EXPECT(req->sql == "SELECT 1");
    }
    
    req = parseJsonRequest("{\"type\":\"begin\"}");
    EXPECT(req.has_value());
    if (req) EXPECT(req->type == "begin");
    
    req = parseJsonRequest("{\"type\":\"commit\",\"txnId\":42}");
    EXPECT(req.has_value());
    if (req) {
        EXPECT(req->type == "commit");
        EXPECT(req->txnId == 42);
    }
    
    // Invalid
    req = parseJsonRequest("{not valid json}");
    EXPECT(!req.has_value());
}