#include <catch2/catch_test_macros.hpp>
#include "meradb/wb_tree.h"

using namespace meradb::wb;

namespace {

nlohmann::ordered_json sample() {
    return nlohmann::ordered_json::parse(R"([
      {"name":"college","current":false,"tables":[]},
      {"name":"main","current":true,"tables":[
         {"name":"students","columns":[
            {"name":"id","type_name":"INT","primary_key":true,"unique":false,"not_null":true},
            {"name":"email","type_name":"TEXT","primary_key":false,"unique":true,"not_null":false},
            {"name":"naam","type_name":"TEXT","primary_key":false,"unique":false,"not_null":true}]},
         {"name":"marks","columns":[{"name":"score","type_name":"FLOAT","primary_key":false,"unique":false,"not_null":false}]}]}])");
}

int rowOf(const TreeModel& m, const std::string& label) {
    for (std::size_t i = 0; i < m.rows().size(); ++i)
        if (m.rows()[i].key.kind != NodeKey::Kind::Root && plainText(m.rows()[i].label).rfind(label, 0) == 0)
            return static_cast<int>(i);
    return -1;
}

std::vector<std::string> labels(const TreeModel& m) {
    std::vector<std::string> out;
    for (const TreeRow& r : m.rows()) out.push_back(plainText(r.label));
    return out;
}

}  // namespace

TEST_CASE("wbtree a fresh model has only the expanded root", "[wbtree]") {
    TreeModel m;
    REQUIRE(m.rows().size() == 1);
    CHECK(plainText(m.rows()[0].label) == "Databases");
    CHECK(m.rows()[0].depth == 0);
    CHECK(m.rows()[0].expanded);
}

TEST_CASE("wbtree refresh shows databases and the current one's tables", "[wbtree]") {
    TreeModel m;
    m.refresh(sample());
    CHECK(labels(m) == std::vector<std::string>({"Databases", "college", "main", "students", "marks"}));
    const TreeRow& college = m.rows()[1];
    CHECK(college.expandable);       // although it is empty
    CHECK_FALSE(college.expanded);
    const TreeRow& mainDb = m.rows()[2];
    CHECK(mainDb.expanded);
    CHECK(mainDb.label[0].style.bold);
    CHECK(mainDb.label[0].style.fg == palette::kGreen);
    CHECK(m.rows()[3].label[0].style.bold);
    CHECK(m.rows()[3].expandable);
    CHECK_FALSE(m.rows()[3].expanded);
}

TEST_CASE("wbtree column labels carry type, key and not-null marks", "[wbtree]") {
    TreeModel m;
    m.refresh(sample());
    auto all = m.allNodes();
    REQUIRE(all.size() == 9);
    std::vector<std::string> cols;
    for (const TreeRow& r : all)
        if (r.depth == 3) cols.push_back(plainText(r.label));
    CHECK(cols == std::vector<std::string>({"id int PK NN", "email text UQ", "naam text NN", "score float"}));
    const TreeRow* id = nullptr;
    for (const TreeRow& r : all)
        if (r.depth == 3 && r.key.column == "id") id = &r;
    REQUIRE(id != nullptr);
    CHECK(id->label[1].style.fg == palette::kCyan);   // the type
    CHECK(id->label[2].style.fg == palette::kOrange); // PK
    CHECK(id->label[3].style.dim);                    // NN
}

TEST_CASE("wbtree expansion survives a refresh", "[wbtree]") {
    TreeModel m;
    m.refresh(sample());
    m.toggle(rowOf(m, "students"));
    CHECK(m.rows().size() == 8);
    m.refresh(sample());
    CHECK(m.rows().size() == 8);
    CHECK(m.rows()[rowOf(m, "students")].expanded);
    // a collapsed current database is opened again by a refresh (tui.py's rule)
    m.toggle(rowOf(m, "main"));
    CHECK(m.rows().size() == 3);
    m.refresh(sample());
    CHECK(rowOf(m, "students") > 0);
}

TEST_CASE("wbtree a refresh re-expands a collapsed root", "[wbtree]") {
    TreeModel m;
    m.refresh(sample());
    m.toggle(0);
    CHECK(m.rows().size() == 1);
    m.refresh(sample());
    CHECK(m.rows()[0].expanded);
    CHECK(labels(m) == std::vector<std::string>({"Databases", "college", "main", "students", "marks"}));
}

TEST_CASE("wbtree selection survives a refresh by key", "[wbtree]") {
    TreeModel m;
    m.refresh(sample());
    m.select(rowOf(m, "marks"));
    m.refresh(sample());
    CHECK(m.rows()[static_cast<std::size_t>(m.selected())].key.table == "marks");
}

TEST_CASE("wbtree activate returns the scripts tui.py runs", "[wbtree]") {
    TreeModel m;
    m.refresh(sample());
    {
        TreeActivation a = m.activate(rowOf(m, "students"), "main");
        CHECK(a.toggled);
        CHECK(a.scripts == std::vector<std::string>({"DIKHAO * SE students SIRF 100;"}));
    }
    {
        TreeActivation a = m.activate(rowOf(m, "students"), "college");
        CHECK(a.scripts == std::vector<std::string>({"ISTEMAL main;", "DIKHAO * SE students SIRF 100;"}));
    }
    {
        TreeActivation a = m.activate(rowOf(m, "college"), "main");
        CHECK(a.toggled);
        CHECK(a.scripts == std::vector<std::string>({"ISTEMAL college;"}));
    }
    {
        TreeActivation a = m.activate(rowOf(m, "main"), "main");
        CHECK(a.toggled);
        CHECK(a.scripts.empty());
    }
    m.refresh(sample());
    m.toggle(rowOf(m, "students"));
    {
        TreeActivation a = m.activate(rowOf(m, "email"), "main");
        CHECK_FALSE(a.toggled);
        CHECK(a.insertText == "email");
        CHECK(a.scripts.empty());
    }
    {
        TreeActivation a = m.activate(0, "main");
        CHECK(a.toggled);
        CHECK(a.scripts.empty());
        CHECK(m.rows().size() == 1);       // the whole tree folds
        TreeActivation b = m.activate(0, "main");
        CHECK(b.toggled);
        CHECK(m.rows().size() > 1);        // and a second activation unfolds it
    }
}

TEST_CASE("wbtree Right and Left expand, collapse and walk the hierarchy", "[wbtree]") {
    TreeModel m;
    m.refresh(sample());
    m.select(rowOf(m, "students"));
    CHECK(m.expandOrDescend());                       // expands
    CHECK(m.rows()[static_cast<std::size_t>(m.selected())].key.table == "students");
    CHECK(m.rows()[static_cast<std::size_t>(m.selected())].expanded);
    CHECK(m.expandOrDescend());                       // now moves to the first child
    CHECK(m.rows()[static_cast<std::size_t>(m.selected())].key.column == "id");
    CHECK_FALSE(m.expandOrDescend());                 // a column has nothing to open
    CHECK(m.collapseOrAscend());                      // up to the table
    CHECK(m.rows()[static_cast<std::size_t>(m.selected())].key.table == "students");
    CHECK(m.collapseOrAscend());                      // collapses the table
    CHECK_FALSE(m.rows()[static_cast<std::size_t>(m.selected())].expanded);
    CHECK(m.collapseOrAscend());                      // up to the database
    CHECK(m.rows()[static_cast<std::size_t>(m.selected())].key.kind == NodeKey::Kind::Database);
    CHECK(m.collapseOrAscend());                      // collapses main
    CHECK(m.rows().size() == 3);
}

TEST_CASE("wbtree a vanished table is forgotten without trouble", "[wbtree]") {
    TreeModel m;
    m.refresh(sample());
    m.toggle(rowOf(m, "marks"));
    auto smaller = nlohmann::ordered_json::parse(
        R"([{"name":"main","current":true,"tables":[{"name":"students","columns":[]}]}])");
    m.refresh(smaller);
    CHECK(labels(m) == std::vector<std::string>({"Databases", "main", "students"}));
    m.refresh(sample());                               // the table comes back collapsed
    CHECK_FALSE(m.rows()[rowOf(m, "marks")].expanded);
}
