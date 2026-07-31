#include "test_framework.hpp"

#include <sstream>
#include <string>

#include "atomdb/contracts/ICommandSource.hpp"
#include "atomdb/core/EngineLoop.hpp"
#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/frontend/ReplSource.hpp"
#include "atomdb/storage/InMemoryStorageEngine.hpp"
#include "atomdb/types/Command.hpp"
#include "atomdb/types/Predicate.hpp"
#include "atomdb/types/Tuple.hpp"

using namespace atomdb;

namespace {

// A scripted ICommandSource for end-to-end dispatch testing.
class ScriptedSource : public ICommandSource {
public:
    std::vector<Command> cmds;
    std::vector<std::string> presented_results;
    std::vector<DbError>  presented_errors;

    std::optional<Command> nextCommand() override {
        if (cursor_ >= cmds.size()) return std::nullopt;
        return cmds[cursor_++];
    }
    void present(const ResultSet& rs) override {
        for (const auto& row : rs.rows) {
            presented_results.push_back(row.toString());
        }
    }
    void present(const DbError& err) override {
        presented_errors.push_back(err);
    }

private:
    std::size_t cursor_ = 0;
};

} // namespace

TEST(Engine_Loop_Insert_Select_Round_Trip) {
    TransactionManager txnm;
    LockManager        lkm;
    DeadlockDetector   dd(lkm);
    InMemoryStorageEngine storage;
    ScriptedSource       src;

    src.cmds.push_back(Command(CommandType::Insert, "u",
        std::nullopt,
        Tuple::make({{"name", Value::text("a")}, {"_id", Value::integer(1)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u",
        std::nullopt,
        Tuple::make({{"name", Value::text("b")}, {"_id", Value::integer(2)}})));
    src.cmds.push_back(Command(CommandType::Select, "u"));

    EngineLoop engine(src, storage, txnm, lkm, dd);
    engine.run();

    EXPECT_EQ(src.presented_errors.size(), std::size_t{0});
    EXPECT_EQ(src.presented_results.size(), std::size_t{2});
    EXPECT(src.presented_results[0] == "name=a,_id=1");
    EXPECT(src.presented_results[1] == "name=b,_id=2");
}

TEST(Engine_Loop_Update_Works) {
    TransactionManager txnm;
    LockManager        lkm;
    DeadlockDetector   dd(lkm);
    InMemoryStorageEngine storage;
    ScriptedSource       src;

    // First insert a row to update
    src.cmds.push_back(Command(CommandType::Insert, "u",
        std::nullopt,
        Tuple::make({{"name", Value::text("a")}, {"_id", Value::integer(1)}})));
    
    // Then update it
    Predicate wh(ComparisonNode::make("_id", ComparisonOp::Equals, Value::integer(1)));
    src.cmds.push_back(Command(
        CommandType::Update, "u",
        std::move(wh),
        Tuple::make({{"name", Value::text("Z")}})));

    EngineLoop engine(src, storage, txnm, lkm, dd);
    engine.run();

    // Should have no errors
    EXPECT_EQ(src.presented_errors.size(), std::size_t{0});
    
    // Verify the update worked by selecting the row
    ScriptedSource src2;
    src2.cmds.push_back(Command(CommandType::Select, "u",
        Predicate(ComparisonNode::make("_id", ComparisonOp::Equals, Value::integer(1)))));
    
    EngineLoop engine2(src2, storage, txnm, lkm, dd);
    engine2.run();
    
    EXPECT_EQ(src2.presented_results.size(), std::size_t{1});
    EXPECT(src2.presented_results[0].find("Z") != std::string::npos);
}

TEST(Engine_Loop_Where_Filter_Narrows_Select) {
    TransactionManager txnm;
    LockManager        lkm;
    DeadlockDetector   dd(lkm);
    InMemoryStorageEngine storage;
    ScriptedSource       src;

    src.cmds.push_back(Command(CommandType::Insert, "u",
        std::nullopt,
        Tuple::make({{"name", Value::text("a")}, {"_id", Value::integer(1)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u",
        std::nullopt,
        Tuple::make({{"name", Value::text("b")}, {"_id", Value::integer(2)}})));
    src.cmds.push_back(Command(
        CommandType::Select, "u",
        Predicate(ComparisonNode::make("name", ComparisonOp::Equals, Value::text("a")))));

    EngineLoop engine(src, storage, txnm, lkm, dd);
    engine.run();

    EXPECT_EQ(src.presented_results.size(), std::size_t{1});
    EXPECT(src.presented_results[0] == "name=a,_id=1");
}

TEST(Repl_Parses_Insert_Then_Select) {
    std::istringstream in(
        "INSERT users {key:1,name:nikhil,age:30}\n"
        "SELECT users\n"
        "EXIT\n");
    std::ostringstream out;
    ReplSource repl(in, out);

    auto c1 = repl.nextCommand();
    EXPECT(c1.has_value());
    EXPECT(c1->type == CommandType::Insert);
    EXPECT(c1->table == "users");
    EXPECT(c1->values.has_value());
    EXPECT(c1->values->get("name").asText() == "nikhil");

    auto c2 = repl.nextCommand();
    EXPECT(c2.has_value());
    EXPECT(c2->type == CommandType::Select);
    EXPECT(c2->table == "users");

    auto c3 = repl.nextCommand();
    EXPECT(!c3.has_value()); // EXIT -> nullopt
}

TEST(Repl_Auto_Hydrates_Present_With_No_Rows_Prints_OK) {
    std::istringstream in("SELECT users\nEXIT\n");
    std::ostringstream out;
    ReplSource repl(in, out);
    ResultSet rs;
    rs.success = true;
    repl.present(rs);
    EXPECT(out.str() == "[OK]\n");
}

TEST(Engine_Loop_OrderBy_Sorts_Rows_Ascending) {
    TransactionManager txnm;
    LockManager        lkm;
    DeadlockDetector   dd(lkm);
    InMemoryStorageEngine storage;
    ScriptedSource       src;

    src.cmds.push_back(Command(CommandType::Insert, "u",
        std::nullopt,
        Tuple::make({{"name", Value::text("charlie")}, {"_id", Value::integer(1)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u",
        std::nullopt,
        Tuple::make({{"name", Value::text("alpha")},   {"_id", Value::integer(2)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u",
        std::nullopt,
        Tuple::make({{"name", Value::text("bravo")},   {"_id", Value::integer(3)}})));

    // Sort by name ASC.
    src.cmds.push_back(Command(CommandType::Select, "u").withOrderBy(
        {{"name", SortDirection::Asc}}));

    EngineLoop engine(src, storage, txnm, lkm, dd);
    engine.run();

    EXPECT_EQ(src.presented_errors.size(), std::size_t{0});
    EXPECT_EQ(src.presented_results.size(), std::size_t{3});
    // Sorted ascending: alpha (id=2), bravo (id=3), charlie (id=1)
    EXPECT(src.presented_results[0].find("_id=2") != std::string::npos);
    EXPECT(src.presented_results[1].find("_id=3") != std::string::npos);
    EXPECT(src.presented_results[2].find("_id=1") != std::string::npos);
}

TEST(Engine_Loop_Limit_Truncates_Rows) {
    TransactionManager txnm;
    LockManager        lkm;
    DeadlockDetector   dd(lkm);
    InMemoryStorageEngine storage;
    ScriptedSource       src;

    for (int i = 1; i <= 10; ++i) {
        src.cmds.push_back(Command(CommandType::Insert, "u",
            std::nullopt,
            Tuple::make({{"_id", Value::int64(i)}, {"name", Value::text("u" + std::to_string(i))}})));
    }
    src.cmds.push_back(Command(CommandType::Select, "u").withLimit(3));

    EngineLoop engine(src, storage, txnm, lkm, dd);
    engine.run();

    EXPECT_EQ(src.presented_results.size(), std::size_t{3});
    // We emit each row separately; first three must be _id 1..3.
    EXPECT(src.presented_results[0].find("_id=1") != std::string::npos);
    EXPECT(src.presented_results[2].find("_id=3") != std::string::npos);
}

// Phase 6.3: NULLS FIRST / NULLS LAST semantics.
//
// Default per PostgreSQL:
//   - ASC  -> NULLS LAST  (NULL sorts after non-NULL)
//   - DESC -> NULLS FIRST (NULL sorts before non-NULL)
//
// Explicit NULLS FIRST / NULLS LAST override the default.

TEST(Engine_Loop_OrderBy_NullsDefault_AscPlacesLast) {
    TransactionManager txnm;
    LockManager        lkm;
    DeadlockDetector   dd(lkm);
    InMemoryStorageEngine storage;
    ScriptedSource       src;

    // rows: id=1 (a=null), id=2 (a=10), id=3 (a=null), id=4 (a=20)
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::null()}, {"_id", Value::int64(1)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::int64(10)}, {"_id", Value::int64(2)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::null()}, {"_id", Value::int64(3)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::int64(20)}, {"_id", Value::int64(4)}})));

    src.cmds.push_back(Command(CommandType::Select, "u").withOrderBy(
        {{"a", SortDirection::Asc, std::nullopt}}));

    EngineLoop engine(src, storage, txnm, lkm, dd);
    engine.run();

    // Expected order: 2 (a=10), 4 (a=20), 1 (a=null), 3 (a=null)
    EXPECT_EQ(src.presented_results.size(), std::size_t{4});
    EXPECT(src.presented_results[0].find("_id=2") != std::string::npos);
    EXPECT(src.presented_results[1].find("_id=4") != std::string::npos);
    EXPECT(src.presented_results[2].find("_id=1") != std::string::npos);
    EXPECT(src.presented_results[3].find("_id=3") != std::string::npos);
}

TEST(Engine_Loop_OrderBy_NullsDefault_DescPlacesFirst) {
    TransactionManager txnm;
    LockManager        lkm;
    DeadlockDetector   dd(lkm);
    InMemoryStorageEngine storage;
    ScriptedSource       src;

    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::int64(10)}, {"_id", Value::int64(1)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::null()}, {"_id", Value::int64(2)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::int64(20)}, {"_id", Value::int64(3)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::null()}, {"_id", Value::int64(4)}})));

    src.cmds.push_back(Command(CommandType::Select, "u").withOrderBy(
        {{"a", SortDirection::Desc, std::nullopt}}));

    EngineLoop engine(src, storage, txnm, lkm, dd);
    engine.run();

    // Expected order: 2 (a=null), 4 (a=null), 3 (a=20), 1 (a=10)
    EXPECT_EQ(src.presented_results.size(), std::size_t{4});
    EXPECT(src.presented_results[0].find("_id=2") != std::string::npos);
    EXPECT(src.presented_results[1].find("_id=4") != std::string::npos);
    EXPECT(src.presented_results[2].find("_id=3") != std::string::npos);
    EXPECT(src.presented_results[3].find("_id=1") != std::string::npos);
}

TEST(Engine_Loop_OrderBy_NullsFirst_Override) {
    TransactionManager txnm;
    LockManager        lkm;
    DeadlockDetector   dd(lkm);
    InMemoryStorageEngine storage;
    ScriptedSource       src;

    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::int64(10)}, {"_id", Value::int64(1)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::null()}, {"_id", Value::int64(2)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::int64(20)}, {"_id", Value::int64(3)}})));

    // ASC + NULLS FIRST: NULLs before non-NULLs.
    src.cmds.push_back(Command(CommandType::Select, "u").withOrderBy(
        {{"a", SortDirection::Asc, std::optional<bool>{true}}}));

    EngineLoop engine(src, storage, txnm, lkm, dd);
    engine.run();

    EXPECT(src.presented_results[0].find("_id=2") != std::string::npos);
    EXPECT(src.presented_results[1].find("_id=1") != std::string::npos);
    EXPECT(src.presented_results[2].find("_id=3") != std::string::npos);
}

TEST(Engine_Loop_OrderBy_NullsLast_Override) {
    TransactionManager txnm;
    LockManager        lkm;
    DeadlockDetector   dd(lkm);
    InMemoryStorageEngine storage;
    ScriptedSource       src;

    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::int64(10)}, {"_id", Value::int64(1)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::null()}, {"_id", Value::int64(2)}})));
    src.cmds.push_back(Command(CommandType::Insert, "u", std::nullopt,
        Tuple::make({{"a", Value::int64(20)}, {"_id", Value::int64(3)}})));

    // DESC + NULLS LAST: NULLs after non-NULLs (override DESC default).
    src.cmds.push_back(Command(CommandType::Select, "u").withOrderBy(
        {{"a", SortDirection::Desc, std::optional<bool>{false}}}));

    EngineLoop engine(src, storage, txnm, lkm, dd);
    engine.run();

    EXPECT(src.presented_results[0].find("_id=3") != std::string::npos);
    EXPECT(src.presented_results[1].find("_id=1") != std::string::npos);
    EXPECT(src.presented_results[2].find("_id=2") != std::string::npos);
}

