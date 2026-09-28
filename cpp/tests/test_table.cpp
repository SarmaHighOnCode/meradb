// cpp/tests/test_table.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/storage.h"
#include "meradb/table.h"
#include "test_util.h"

using namespace meradb;
using meradb_test::TempDir;

namespace {
TableSchema makeSchema() {
    TableSchema s;
    s.name = "t";
    Column id; id.name = "id"; id.typeName = "INT"; id.primaryKey = true;
    Column naam; naam.name = "naam"; naam.typeName = "TEXT";
    s.columns = {id, naam};
    return s;
}

// enroll(sid INT, cid INT, email TEXT ANOKHA, MUKHYA KUNJI (sid, cid))
TableSchema makeCompositeSchema() {
    TableSchema s;
    s.name = "enroll";
    Column sid; sid.name = "sid"; sid.typeName = "INT";
    Column cid; cid.name = "cid"; cid.typeName = "INT";
    Column email; email.name = "email"; email.typeName = "TEXT"; email.unique = true;
    s.columns = {sid, cid, email};
    s.compositePk = std::vector<std::string>{"sid", "cid"};
    return s;
}

Value I(int64_t v) { return Value(v); }
Value S(const char* v) { return Value(std::string(v)); }
}  // namespace

TEST_CASE("Table inserts and scans rows", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}, {I(2), S("Priya")}});
    auto rows = t.rows();
    REQUIRE(rows.size() == 2);
    REQUIRE(std::get<std::string>(rows[1].second[1].data) == "Priya");
    REQUIRE(std::get<std::string>(t.get(rows[0].first).value()[1].data) == "Ravi");
}

TEST_CASE("Table.lookup uses the primary-key hash index", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}, {I(2), S("Priya")}});
    auto hits = t.lookup(0, I(2));
    REQUIRE(hits.size() == 1);
    REQUIRE(std::get<std::string>(hits[0].second[1].data) == "Priya");
    REQUIRE(t.lookup(0, I(999)).empty());
    REQUIRE(t.lookup(0, Value()).empty());
    // unindexed column: an error, never a silently empty answer
    REQUIRE_THROWS_AS(t.lookup(1, S("Ravi")), ExecutionError);
}

TEST_CASE("Index keys follow Python dict semantics: 2 == 2.0 == value, SACH == 1", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}, {I(2), S("Priya")}});
    REQUIRE(t.lookup(0, Value(2.0)).size() == 1);
    REQUIRE(t.lookup(0, Value(true)).size() == 1);
    REQUIRE(t.lookup(0, Value(1.5)).empty());
    REQUIRE(t.lookup(0, S("2")).empty());

    ValueVecHash h;
    ValueVecEq eq;
    REQUIRE(eq({I(2)}, {Value(2.0)}));
    REQUIRE(h({I(2)}) == h({Value(2.0)}));
    REQUIRE(eq({Value(-0.0)}, {I(0)}));
    REQUIRE_FALSE(eq({I(9007199254740993)}, {Value(9007199254740992.0)}));
    REQUIRE_FALSE(eq({S("a")}, {Value(parseDate("2024-01-01"))}));
    REQUIRE(eq({Value(parseDate("2024-01-01"))}, {Value(parseDate("2024-01-01"))}));
}

TEST_CASE("Table.deleteMany removes rows and updates the index", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}});
    t.indexes();  // built before the delete, so deleteMany must keep it in sync
    t.deleteMany(t.rows());
    REQUIRE(t.rows().empty());
    REQUIRE(t.lookup(0, I(1)).empty());
    REQUIRE(t.indexes().at({0}).empty());
}

TEST_CASE("deleteMany keeps an index entry that points at a newer row (UPDATE)", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}});
    t.indexes();
    auto old = t.rows();
    t.insertMany({{I(1), S("Ravi Kumar")}});  // new version of the row first...
    t.deleteMany(old);                          // ...then the old one goes
    auto hits = t.lookup(0, I(1));
    REQUIRE(hits.size() == 1);
    REQUIRE(std::get<std::string>(hits[0].second[1].data) == "Ravi Kumar");
}

TEST_CASE("Table.invalidateIndexes forces a rebuild on next access", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    Table t(makeSchema(), path);
    t.insertMany({{I(1), S("Ravi")}});
    REQUIRE(t.lookup(0, I(1)).size() == 1);  // builds the index
    t.heap().rewrite({encodeRow({I(5), S("Asha")}, t.schema().types())});  // row ids change
    t.invalidateIndexes();
    REQUIRE(t.lookup(0, I(1)).empty());
    REQUIRE(t.lookup(0, I(5)).size() == 1);  // rebuilt transparently
}

TEST_CASE("Composite and UNIQUE indexes skip KHALI and track inserts", "[table]") {
    TempDir dir;
    std::string path = dir.file("enroll.tbl");
    HeapFile(path).create();
    Table t(makeCompositeSchema(), path);
    t.insertMany({{I(1), I(10), S("a@x")}, {I(1), I(11), Value()}, {I(2), Value(), S("c@x")}});
    IndexMap& idx = t.indexes();
    REQUIRE(idx.size() == 2);  // email {2} + composite pk {0, 1}
    REQUIRE(idx.at({2}).size() == 2);     // the KHALI email is not indexed
    REQUIRE(idx.at({0, 1}).size() == 2);  // (2, KHALI) is not indexed
    REQUIRE(idx.at({0, 1}).count({I(1), I(11)}) == 1);

    t.insertMany({{I(3), I(30), S("d@x")}});  // index already built: updated in place
    REQUIRE(idx.at({0, 1}).count({I(3), I(30)}) == 1);
    REQUIRE(idx.at({2}).count({S("d@x")}) == 1);
}

TEST_CASE("Tables sharing an IndexCache see one index", "[table]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    HeapFile(path).create();
    auto cache = std::make_shared<std::optional<IndexMap>>();
    {
        Table t(makeSchema(), path, cache);
        t.insertMany({{I(1), S("Ravi")}});
        t.indexes();
    }
    REQUIRE(cache->has_value());
    Table t2(makeSchema(), path, cache);
    t2.insertMany({{I(2), S("Priya")}});
    REQUIRE((*cache)->at({0}).size() == 2);
    t2.invalidateIndexes();
    REQUIRE_FALSE(cache->has_value());
}

TEST_CASE("MaterializedTable behaves like Table for reads, is flagged as a view", "[table]") {
    std::vector<RowValues> data = {{I(1)}, {I(2)}};
    MaterializedTable mt(data);
    REQUIRE(mt.isView());
    REQUIRE(mt.rows().size() == 2);
    REQUIRE(std::get<int64_t>(mt.get(1).value()[0].data) == 2);
    REQUIRE_FALSE(mt.get(2).has_value());
    REQUIRE_FALSE(mt.get(-1).has_value());
    REQUIRE_THROWS_AS(mt.insertMany({{I(3)}}), ExecutionError);
    REQUIRE_THROWS_AS(mt.lookup(0, I(1)), ExecutionError);

    Table t(makeSchema(), "unused.tbl");
    REQUIRE_FALSE(t.isView());
}
