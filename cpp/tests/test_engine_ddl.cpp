// cpp/tests/test_engine_ddl.cpp -- DDL + transactions. Expected texts were
// produced by running the same statements through the Python engine.
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "golden_runner.h"
#include "meradb/parser.h"
#include "test_util.h"
#include <filesystem>

using namespace meradb;
using meradb_test::TempDir;
namespace fs = std::filesystem;

namespace {

Result runOne(Engine& e, const std::string& sql) {
    auto stmts = parseScript(sql);
    return e.executeStatement(*stmts[0]);
}

// The error text of a failing statement ("" if it succeeded).
std::string errorOf(Engine& e, const std::string& sql) {
    auto results = e.runScript(sql);
    return results.at(0).error;
}

std::vector<std::string> rowStrings(const Result& r) {
    std::vector<std::string> out;
    for (const auto& row : r.rows) {
        std::string line;
        for (size_t i = 0; i < row.size(); ++i) line += (i ? "|" : "") + formatValue(row[i]);
        out.push_back(line);
    }
    return out;
}

using Lines = std::vector<std::string>;

Value I(int64_t v) { return Value(v); }
Value S(const std::string& v) { return Value(v); }

// Rows go straight into the heap file: DML is a later task.
void putRows(Engine& e, const std::string& name, const std::vector<std::vector<Value>>& rows) {
    Table t(e.catalog().get(name), e.catalog().tablePath(name));
    t.insertMany(rows);
}
std::vector<StoredRow> readRows(Engine& e, const std::string& name) {
    Table t(e.catalog().get(name), e.catalog().tablePath(name));
    return t.rows();
}

}  // namespace

TEST_CASE("CREATE TABLE validates single primary key and creates the heap file", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    auto r = runOne(e, "BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT ZAROORI);");
    REQUIRE(r.message == "Table 'students' ban gaya (2 columns)");
    REQUIRE(e.catalog().find("students") != nullptr);
    REQUIRE(fs::exists(e.catalog().tablePath("students")));
}

TEST_CASE("CREATE TABLE rejects both single and composite primary key", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE_THROWS_AS(runOne(e, "BANAO TABLE t (id INT MUKHYA KUNJI, b INT, MUKHYA KUNJI (id, b));"), ExecutionError);
    REQUIRE(errorOf(e, "BANAO TABLE t (id INT MUKHYA KUNJI, b INT, MUKHYA KUNJI (id, b));") ==
            "[Execution Galti] Ek table mein sirf ek MUKHYA KUNJI ho sakti hai (single- ya multi-column, dono nahi)");
    REQUIRE(e.catalog().find("t") == nullptr);  // nothing was created
    REQUIRE_FALSE(fs::exists(e.catalog().tablePath("t")));
}

TEST_CASE("CREATE TABLE validation messages", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    runOne(e, "BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT ANOKHA);");
    REQUIRE_NOTHROW(runOne(e, "BANAO TABLE students (id INT MUKHYA KUNJI, cid INT SANDARBH courses(id));"));
    REQUIRE(errorOf(e, "BANAO TABLE bad (id INT MUKHYA KUNJI, cid INT SANDARBH courses(title));") ==
            "[Execution Galti] Column 'cid' (INT) aur SANDARBH 'courses.title' (TEXT) ke types match nahi karte");
    REQUIRE(errorOf(e, "BANAO TABLE bad2 (id INT MUKHYA KUNJI, id INT);") ==
            "[Execution Galti] Column naam do baar diya: id");
    REQUIRE(errorOf(e, "BANAO TABLE bad4 (a INT, b INT, ANOKHA (a, c));") ==
            "[Execution Galti] Table 'bad4': composite constraint mein column 'c' nahi hai");
    REQUIRE(errorOf(e, "BANAO TABLE bad5 (a INT SHART (b > 1));") ==
            "[Execution Galti] Column 'a': SHART mein column 'b' table 'bad5' mein nahi hai");
    REQUIRE(errorOf(e, "BANAO TABLE students (a INT);") == "[Execution Galti] Table 'students' pehle se hai");
}

TEST_CASE("DESCRIBE and SHOW TABLES", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    runOne(e, "BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT ANOKHA);");
    runOne(e,
           "BANAO TABLE students (id INT MUKHYA KUNJI, naam VARCHAR(5) ZAROORI, cid INT SANDARBH courses(id), "
           "age INT WARNA 18 SHART (age > 0));");
    auto d = runOne(e, "BATAO students;");
    REQUIRE(d.columns == std::vector<std::string>{"column", "type", "constraints"});
    REQUIRE(rowStrings(d) == Lines{"id|INT|MUKHYA KUNJI", "naam|TEXT(5)|ZAROORI", "cid|INT|SANDARBH courses(id)",
                                   "age|INT|WARNA 18 SHART (age > 0)"});
    REQUIRE(d.message == "Table 'students'");
    auto t = runOne(e, "DIKHAO TABLES;");
    REQUIRE(t.columns == std::vector<std::string>{"table"});
    REQUIRE(rowStrings(t) == Lines{"courses", "students"});
    REQUIRE(t.message == "2 table(s) in 'main'");
}

TEST_CASE("DROP TABLE refuses when another table's FK references it", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    runOne(e, "BANAO TABLE courses (id INT MUKHYA KUNJI);");
    runOne(e, "BANAO TABLE students (id INT MUKHYA KUNJI, cid INT SANDARBH courses(id));");
    REQUIRE_THROWS_AS(runOne(e, "HATAO TABLE courses;"), ExecutionError);
    REQUIRE(errorOf(e, "HATAO TABLE courses;") ==
            "[Execution Galti] Table 'courses' hata nahi sakte -- students ise SANDARBH karte hain");
    REQUIRE(runOne(e, "HATAO TABLE students;").message == "Table 'students' hata diya");
    REQUIRE_FALSE(fs::exists(e.catalog().tablePath("students")));
    REQUIRE_NOTHROW(runOne(e, "HATAO TABLE courses;"));
}

TEST_CASE("ALTER TABLE ADD COLUMN backfills existing rows with the default", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    runOne(e, "BANAO TABLE t (id INT MUKHYA KUNJI);");
    putRows(e, "t", {{I(1)}, {I(2)}});
    REQUIRE(runOne(e, "SUDHARO TABLE t JODO COLUMN active BOOL WARNA SACH;").message ==
            "Column 'active' 't' mein jod diya");
    auto rows = readRows(e, "t");
    REQUIRE(rows.size() == 2);
    REQUIRE(std::holds_alternative<bool>(rows[0].second[1].data));
    REQUIRE(std::get<bool>(rows[0].second[1].data) == true);
    REQUIRE(errorOf(e, "SUDHARO TABLE t JODO COLUMN k INT ANOKHA WARNA 3;") ==
            "[Execution Galti] Naya ANOKHA column 'k' sab rows mein ek hi WARNA value nahi le sakta");
    REQUIRE(errorOf(e, "SUDHARO TABLE t JODO COLUMN z INT ZAROORI;") ==
            "[Execution Galti] Table khali nahi hai; naya ZAROORI column 'z' ko WARNA value chahiye");
    REQUIRE(errorOf(e, "SUDHARO TABLE t JODO COLUMN active INT;") == "[Execution Galti] Column 'active' pehle se hai");
}

TEST_CASE("ALTER TABLE DROP COLUMN", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    runOne(e, "BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT);");
    runOne(e, "BANAO TABLE students (id INT MUKHYA KUNJI, cid INT SANDARBH courses(id));");
    putRows(e, "courses", {{I(1), S("x")}, {I(2), S("y")}});
    REQUIRE(errorOf(e, "SUDHARO TABLE courses HATAO COLUMN id;") ==
            "[Execution Galti] Column 'id' hata nahi sakte -- students.cid (SANDARBH) ise use karte hain");
    REQUIRE(runOne(e, "SUDHARO TABLE courses HATAO COLUMN title;").message == "Column 'title' 'courses' se hata diya");
    auto rows = readRows(e, "courses");
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[1].second.size() == 1);
    REQUIRE(errorOf(e, "SUDHARO TABLE courses HATAO COLUMN id;") ==
            "[Execution Galti] Table ka aakhri column nahi hata sakte -- HATAO TABLE use karo");
}

TEST_CASE("RENAME TABLE cascades into other tables' FK metadata", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    runOne(e, "BANAO TABLE courses (id INT MUKHYA KUNJI);");
    runOne(e, "BANAO TABLE students (id INT MUKHYA KUNJI, cid INT SANDARBH courses(id));");
    REQUIRE(errorOf(e, "SUDHARO TABLE courses NAYA_NAAM courses;") ==
            "[Execution Galti] Naya naam purane naam 'courses' jaisa hi hai");
    REQUIRE(errorOf(e, "SUDHARO TABLE students NAYA_NAAM courses;") ==
            "[Execution Galti] Table 'courses' pehle se hai");
    runOne(e, "SUDHARO TABLE courses NAYA_NAAM classes;");
    REQUIRE(e.catalog().get("students").columns[1].refTable.value() == "classes");
    REQUIRE(fs::exists(e.catalog().tablePath("classes")));
    REQUIRE_FALSE(fs::exists(e.catalog().tablePath("courses")));
}

TEST_CASE("RENAME COLUMN cascades into FK metadata", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    runOne(e, "BANAO TABLE courses (id INT MUKHYA KUNJI);");
    runOne(e, "BANAO TABLE students (id INT MUKHYA KUNJI, cid INT SANDARBH courses(id));");
    runOne(e, "SUDHARO TABLE courses COLUMN id NAYA_NAAM cid2;");
    REQUIRE(e.catalog().get("students").columns[1].refColumn.value() == "cid2");
}

TEST_CASE("Composite ANOKHA on existing data, SAAF, SIKODO", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    runOne(e, "BANAO TABLE pairs (a INT, b INT);");
    putRows(e, "pairs", {{I(1), I(2)}, {I(1), I(2)}});
    REQUIRE(errorOf(e, "SUDHARO TABLE pairs JODO ANOKHA (a, b);") ==
            "[Execution Galti] Duplicate value (1, 2) columns ['a', 'b'] mein -- maujooda data ye constraint todta hai");
    REQUIRE(errorOf(e, "SUDHARO TABLE pairs JODO MUKHYA KUNJI (a, zz);") ==
            "[Execution Galti] Table 'pairs': composite constraint mein column 'zz' nahi hai");
    // a failed ALTER leaves the catalog untouched
    REQUIRE(e.catalog().get("pairs").compositeUnique.empty());
    REQUIRE(runOne(e, "SAAF TABLE pairs;").message == "Table 'pairs' saaf -- 2 row(s) hataye");
    REQUIRE(runOne(e, "SUDHARO TABLE pairs JODO ANOKHA (a, b);").message ==
            "Table 'pairs' mein ANOKHA (a, b) jod diya");
    REQUIRE(e.catalog().get("pairs").compositeUnique.size() == 1);
    REQUIRE(runOne(e, "SIKODO TABLE pairs;").message == "Table 'pairs' sikod diya: 8 -> 8 bytes (0 bytes bache)");
}

TEST_CASE("Databases: create, use, drop", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE(runOne(e, "BANAO DATABASE school;").message == "Database 'school' ban gaya");
    REQUIRE(errorOf(e, "BANAO DATABASE school;") == "[Execution Galti] Database 'school' pehle se hai");
    REQUIRE(runOne(e, "ISTEMAL school;").message == "Ab database 'school' istemal ho raha hai");
    REQUIRE(e.currentDb == "school");
    runOne(e, "BANAO TABLE t (id INT);");
    REQUIRE(runOne(e, "HATAO DATABASE school;").message == "Database 'school' hata diya");
    REQUIRE(e.currentDb == "main");
    REQUIRE(errorOf(e, "HATAO DATABASE main;") == "[Execution Galti] 'main' default database hai, use hata nahi sakte");
    REQUIRE(errorOf(e, "ISTEMAL nahi_hai;") == "[Execution Galti] Database 'nahi_hai' exist nahi karta");
}

TEST_CASE("BEGIN/COMMIT/ROLLBACK toggle inTransaction and persist/discard changes", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    runOne(e, "BANAO TABLE t (id INT MUKHYA KUNJI);");
    REQUIRE_FALSE(e.inTransaction());
    REQUIRE(runOne(e, "SHURU;").message == "Transaction SHURU. PAKKA se save karo, WAPAS se sab undo.");
    REQUIRE(e.inTransaction());
    runOne(e, "HATAO TABLE t;");
    runOne(e, "BANAO TABLE u (id INT);");
    REQUIRE(runOne(e, "WAPAS;").message == "Transaction WAPAS -- saare changes undo ho gaye");
    REQUIRE_FALSE(e.inTransaction());
    REQUIRE(e.catalog().find("t") != nullptr);  // the drop was rolled back
    REQUIRE(e.catalog().find("u") == nullptr);

    runOne(e, "SHURU;");
    runOne(e, "BANAO TABLE u (id INT);");
    REQUIRE(runOne(e, "PAKKA;").message == "Transaction PAKKA -- saare changes save ho gaye");
    REQUIRE(e.catalog().find("u") != nullptr);
    REQUIRE(errorOf(e, "PAKKA;") == "[Execution Galti] Koi transaction nahi chal raha (SHURU se shuru karo)");
    REQUIRE(errorOf(e, "WAPAS;") == "[Execution Galti] Koi transaction nahi chal raha (SHURU se shuru karo)");
}

TEST_CASE("A nested BEGIN while already in a transaction throws; DB commands are refused", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    runOne(e, "SHURU;");
    REQUIRE_THROWS_AS(runOne(e, "SHURU;"), ExecutionError);
    REQUIRE(errorOf(e, "BANAO DATABASE x;") ==
            "[Execution Galti] BANAO DATABASE transaction ke andar nahi chal sakta -- pehle PAKKA ya WAPAS karo");
    REQUIRE(errorOf(e, "ISTEMAL main;") ==
            "[Execution Galti] ISTEMAL transaction ke andar nahi chal sakta -- pehle PAKKA ya WAPAS karo");
    e.close();
    REQUIRE_FALSE(e.inTransaction());
}

TEST_CASE("runScript keeps going after an error and reports it", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    auto results = e.runScript("BANAO TABLE t (id INT); BANAO TABLE t (id INT); DIKHAO TABLES;");
    REQUIRE(results.size() == 3);
    REQUIRE(results[0].error.empty());
    REQUIRE(results[1].error == "[Execution Galti] Table 't' pehle se hai");
    REQUIRE(results[2].message == "1 table(s) in 'main'");
    auto bad = e.runScript("BANAO BANAO;");
    REQUIRE(bad.size() == 1);
    REQUIRE_FALSE(bad[0].error.empty());
}

TEST_CASE("golden transaction scripts match the Python engine", "[engine][ddl][golden]") {
    meradb_test::replayGolden("txn");
}

TEST_CASE("An open transaction is rolled back by crash recovery at the next start", "[engine][ddl][recovery]") {
    TempDir dir;
    {
        Engine e(dir.str());
        e.execute("BANAO TABLE t (id INT MUKHYA KUNJI); DAALO MEIN t MAAN (1);");
        e.execute("SHURU; DAALO MEIN t MAAN (2); BANAO TABLE u (a INT);");
        REQUIRE(e.inTransaction());
        // dropped WITHOUT close(): the snapshot stays on disk, like a crash
    }
    REQUIRE(fs::is_directory(dir.path() / ".wapas" / "main"));
    Engine e2(dir.str());
    REQUIRE(e2.instance().recovered() == std::vector<std::string>{"main"});
    REQUIRE_FALSE(e2.inTransaction());
    auto r = e2.execute("DIKHAO * SE t;");
    REQUIRE(rowStrings(r[0]) == Lines{"1"});
    REQUIRE(e2.catalog().find("u") == nullptr);
    REQUIRE_FALSE(fs::exists(dir.path() / ".wapas" / "main"));
}

TEST_CASE("close() rolls back an open transaction and frees the lock", "[engine][ddl][recovery]") {
    TempDir dir;
    auto instance = std::make_shared<Instance>(dir.str());
    Engine e(instance);
    e.execute("BANAO TABLE t (id INT); SHURU; DAALO MEIN t MAAN (1);");
    e.close();
    REQUIRE_FALSE(e.inTransaction());
    REQUIRE(e.execute("DIKHAO * SE t;")[0].rows.empty());
    Engine other(instance);  // a second session on the same Instance can work again
    REQUIRE(other.execute("DAALO MEIN t MAAN (5);")[0].message == "1 row(s) daal di");
}

TEST_CASE("A stray std::exception becomes a StorageError, not an escape", "[engine][ddl]") {
    TempDir dir;
    Engine e(dir.str());
    e.execute("BANAO TABLE t (id INT);");
    fs::remove(e.catalog().tablePath("t"));  // vanished heap file: fs::file_size would throw
    auto results = e.runScript("SIKODO TABLE t;");
    REQUIRE(results.size() == 1);
    REQUIRE_FALSE(results[0].error.empty());
    REQUIRE_THROWS_AS(e.execute("SIKODO TABLE t;"), MeraDBError);
}
