#include "atomdb/frontend/SqlParser.hpp"
#include "atomdb/types/Value.hpp"
#include "atomdb/types/Predicate.hpp"
#include "atomdb/types/Schema.hpp"
#include "../tests/test_framework.hpp"

using namespace atomdb;

TEST(SqlParser_CreateTable_Basic) {
    SqlParser p;
    auto s = p.parse("CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT)");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* ddl = std::get_if<DdlCreateTable>(&*s);
        EXPECT(ddl != nullptr);
        if (ddl) {
            EXPECT(ddl->schema.table == "users");
            EXPECT(ddl->schema.columns.size() == 3);
            EXPECT(ddl->schema.columns[0].name == "id");
            EXPECT(ddl->schema.columns[0].type == ValueType::Int64);
            EXPECT(ddl->schema.columns[0].primaryKey);
            EXPECT(ddl->schema.columns[1].name == "name");
            EXPECT(ddl->schema.columns[1].type == ValueType::Text);
        }
    }
}

TEST(SqlParser_CreateTable_AllTypes) {
    SqlParser p;
    auto s = p.parse("CREATE TABLE t (a INT, b BIGINT, c SMALLINT, d DOUBLE, e TEXT, f BLOB, g DATE, h TIMESTAMP, i BOOLEAN)");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* ddl = std::get_if<DdlCreateTable>(&*s);
        EXPECT(ddl != nullptr);
        if (ddl) {
            EXPECT(ddl->schema.columns.size() == 9);
            EXPECT(ddl->schema.columns[0].type == ValueType::Int64);
            EXPECT(ddl->schema.columns[1].type == ValueType::Int64);
            EXPECT(ddl->schema.columns[2].type == ValueType::Int32);
            EXPECT(ddl->schema.columns[3].type == ValueType::Double);
            EXPECT(ddl->schema.columns[4].type == ValueType::Text);
            EXPECT(ddl->schema.columns[5].type == ValueType::Blob);
            EXPECT(ddl->schema.columns[6].type == ValueType::Date);
            EXPECT(ddl->schema.columns[7].type == ValueType::Timestamp);
            EXPECT(ddl->schema.columns[8].type == ValueType::Bool);
        }
    }
}

TEST(SqlParser_CreateTable_PartitionedByHash) {
    SqlParser p;
    auto s = p.parse("CREATE TABLE u (id INT PRIMARY KEY, name TEXT) PARTITION BY HASH(id) PARTITIONS 4");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* ddl = std::get_if<DdlCreateTable>(&*s);
        EXPECT(ddl != nullptr);
        if (ddl) {
            EXPECT(ddl->schema.partition.has_value());
            if (ddl->schema.partition) {
                EXPECT(ddl->schema.partition->kind == PartitionPolicy::Kind::Hash);
                EXPECT(ddl->schema.partition->column == "id");
                EXPECT(ddl->schema.partition->shardCount == 4);
            }
        }
    }
}

TEST(SqlParser_DropTable) {
    SqlParser p;
    auto s = p.parse("DROP TABLE users");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* ddl = std::get_if<DdlDropTable>(&*s);
        EXPECT(ddl != nullptr);
        if (ddl) {
            EXPECT(ddl->table == "users");
        }
    }
}

TEST(SqlParser_Insert_Values) {
    SqlParser p;
    auto s = p.parse("INSERT INTO users VALUES (1, 'nikhil', 30)");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->type == CommandType::Insert);
            EXPECT(cmd->table == "users");
            EXPECT(cmd->values.has_value());
            if (cmd->values) {
                EXPECT(cmd->values->size() == 3);
                EXPECT(cmd->values->get("0") == Value::int64(1));
                EXPECT(cmd->values->get("1") == Value::text("nikhil"));
                EXPECT(cmd->values->get("2") == Value::int64(30));
            }
        }
    }
}

TEST(SqlParser_Insert_WithColumns) {
    SqlParser p;
    auto s = p.parse("INSERT INTO users (id, name) VALUES (42, 'alice')");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->values.has_value());
            if (cmd->values) {
                EXPECT(cmd->values->size() == 2);
            }
        }
    }
}

TEST(SqlParser_Select_Star) {
    SqlParser p;
    auto s = p.parse("SELECT * FROM users");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->type == CommandType::Select);
            EXPECT(cmd->table == "users");
            EXPECT(cmd->projections.size() == 1);
            EXPECT(cmd->projections[0] == "*");
        }
    }
}

TEST(SqlParser_Select_WithProjection) {
    SqlParser p;
    auto s = p.parse("SELECT id, name FROM users");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->projections.size() == 2);
            EXPECT(cmd->projections[0] == "id");
            EXPECT(cmd->projections[1] == "name");
        }
    }
}

TEST(SqlParser_Select_WithWhere) {
    SqlParser p;
    auto s = p.parse("SELECT * FROM users WHERE age > 30");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->where.has_value());
            if (cmd->where) {
                EXPECT(!cmd->where->empty());
            }
        }
    }
}

TEST(SqlParser_Select_WithWhereAndPred) {
    SqlParser p;
    auto s = p.parse("SELECT * FROM users WHERE age > 25 AND name = 'nikhil'");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->where.has_value());
        }
    }
}

TEST(SqlParser_Update_Set) {
    SqlParser p;
    auto s = p.parse("UPDATE users SET name = 'bob' WHERE id = 1");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->type == CommandType::Update);
            EXPECT(cmd->values.has_value());
            if (cmd->values) {
                EXPECT(cmd->values->size() == 1);
                EXPECT(cmd->values->get("name") == Value::text("bob"));
            }
            EXPECT(cmd->where.has_value());
        }
    }
}

TEST(SqlParser_Update_SetMultiple) {
    SqlParser p;
    auto s = p.parse("UPDATE users SET name = 'bob', age = 31 WHERE id = 1");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->values.has_value());
            if (cmd->values) {
                EXPECT(cmd->values->size() == 2);
            }
        }
    }
}

TEST(SqlParser_Delete) {
    SqlParser p;
    auto s = p.parse("DELETE FROM users WHERE id = 1");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->type == CommandType::Delete);
            EXPECT(cmd->where.has_value());
        }
    }
}

TEST(SqlParser_Begin) {
    SqlParser p;
    auto s = p.parse("BEGIN");
    EXPECT(s.has_value());
    if (s.has_value()) {
        EXPECT(std::get_if<SqlTxnBegin>(&*s) != nullptr);
    }
}

TEST(SqlParser_Commit) {
    SqlParser p;
    auto s = p.parse("COMMIT");
    EXPECT(s.has_value());
    if (s.has_value()) {
        EXPECT(std::get_if<SqlTxnCommit>(&*s) != nullptr);
    }
}

TEST(SqlParser_Rollback) {
    SqlParser p;
    auto s = p.parse("ROLLBACK");
    EXPECT(s.has_value());
    if (s.has_value()) {
        EXPECT(std::get_if<SqlTxnRollback>(&*s) != nullptr);
    }
}

TEST(SqlParser_Begin_Transaction_Keyword) {
    SqlParser p;
    auto s = p.parse("BEGIN TRANSACTION");
    EXPECT(s.has_value());
    if (s.has_value()) {
        EXPECT(std::get_if<SqlTxnBegin>(&*s) != nullptr);
    }
}

TEST(SqlParser_ParseAll_MultipleStatements) {
    SqlParser p;
    auto stmts = p.parseAll("CREATE TABLE u(id INT); INSERT INTO u VALUES(1); SELECT * FROM u;");
    EXPECT(stmts.size() == 3);
    if (stmts.size() == 3) {
        EXPECT(std::get_if<DdlCreateTable>(&stmts[0]) != nullptr);
        EXPECT(std::get_if<Command>(&stmts[1]) != nullptr);
        EXPECT(std::get_if<Command>(&stmts[2]) != nullptr);
    }
}

TEST(SqlParser_CaseInsensitive_Keywords) {
    SqlParser p;
    auto s1 = p.parse("create table u (id int primary key)");
    EXPECT(s1.has_value());
    auto s2 = p.parse("select * from u where id = 1");
    EXPECT(s2.has_value());
}

TEST(SqlParser_InvalidStatement_Error) {
    SqlParser p;
    auto s = p.parse("INVALID VERB user");
    EXPECT(!s.has_value());
    EXPECT(!p.error().empty());
}

TEST(SqlParser_FloatLiteral) {
    SqlParser p;
    auto s = p.parse("INSERT INTO t VALUES (3.14, -1.5e2, 100)");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        if (cmd && cmd->values) {
            EXPECT(cmd->values->get("0") == Value::real(3.14));
            EXPECT(cmd->values->get("1") == Value::real(-150.0));
            EXPECT(cmd->values->get("2") == Value::int64(100));
        }
    }
}

TEST(SqlParser_BoolNullLiterals) {
    SqlParser p;
    auto s = p.parse("INSERT INTO t VALUES (true, false, null)");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        if (cmd && cmd->values) {
            EXPECT(cmd->values->get("0") == Value::boolean(true));
            EXPECT(cmd->values->get("1") == Value::boolean(false));
            EXPECT(cmd->values->get("2") == Value::null());
        }
    }
}

TEST(SqlParser_CreateTable_NotNullConstraint) {
    SqlParser p;
    auto s = p.parse("CREATE TABLE u (id INT NOT NULL PRIMARY KEY, name TEXT NOT NULL)");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* ddl = std::get_if<DdlCreateTable>(&*s);
        if (ddl) {
            EXPECT(!ddl->schema.columns[0].nullable);
            EXPECT(!ddl->schema.columns[1].nullable);
        }
    }
}

TEST(SqlParser_LineCommentIgnored) {
    SqlParser p;
    auto s = p.parse("-- comment\nSELECT * FROM u; -- another");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->table == "u");
        }
    }
}

TEST(SqlParser_WhereOrExpr) {
    SqlParser p;
    auto s = p.parse("SELECT * FROM u WHERE a = 1 OR b = 2");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        if (cmd) {
            EXPECT(cmd->where.has_value());
        }
    }
}
TEST(SqlParser_OrderBy_Asc_Populates_Command) {
    SqlParser p;
    auto s = p.parse("SELECT * FROM u ORDER BY name ASC");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT(cmd != nullptr);
        EXPECT_EQ(cmd->orderBy.size(), std::size_t{1});
        EXPECT(cmd->orderBy[0].column == "name");
        EXPECT(cmd->orderBy[0].direction == SortDirection::Asc);
    }
}

TEST(SqlParser_OrderBy_Multi_Columns) {
    SqlParser p;
    auto s = p.parse("SELECT * FROM u ORDER BY age DESC, name ASC");
    EXPECT(s.has_value());
    if (s.has_value()) {
        auto* cmd = std::get_if<Command>(&*s);
        EXPECT_EQ(cmd->orderBy.size(), std::size_t{2});
        EXPECT(cmd->orderBy[0].column == "age");
        EXPECT(cmd->orderBy[0].direction == SortDirection::Desc);
        EXPECT(cmd->orderBy[1].column == "name");
        EXPECT(cmd->orderBy[1].direction == SortDirection::Asc);
    }
}

TEST(SqlParser_Limit_And_Offset_Populate_Command) {
    SqlParser p;
    auto s = p.parse("SELECT * FROM u LIMIT 10 OFFSET 5");
    EXPECT(s.has_value());
    auto* cmd = std::get_if<Command>(&*s);
    EXPECT(cmd != nullptr);
    EXPECT(cmd->limit.has_value()  && *cmd->limit  == std::size_t{10});
    EXPECT(cmd->offset.has_value() && *cmd->offset == std::size_t{5});
}
