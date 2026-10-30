// cpp/tests/test_security.cpp -- proofs of concept for security flaws found in review. Each case failed before its fix.
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"
#include "meradb/engine.h"
#include "test_util.h"

using namespace meradb;
using namespace meradb_test;

namespace {

struct SecFixture {
    TempDir dir;
    std::shared_ptr<Instance> inst = std::make_shared<Instance>(dir.str());
    Engine admin{inst};
    Engine ravi{inst};
    SecFixture() {
        ravi.user = "ravi";
        admin.execute("BANAO USER ravi GUPT 'pw';"
                      "BANAO TABLE pub (id INT MUKHYA KUNJI, name TEXT, secret INT);"
                      "BANAO TABLE hidden (id INT);"
                      "DAALO MEIN pub MAAN (1, 'a', 30), (2, 'b', 10), (3, 'c', 20);"
                      "DAALO MEIN hidden MAAN (777);");
    }
    std::string nameOf(int id) {
        return formatValue(runLast(admin, "DIKHAO name SE pub JAHAN id = " + std::to_string(id) + ";").rows.at(0).at(0));
    }
};

}  // namespace

// ---- privileges: statements that read or write more than their check looked at ----

TEST_CASE("security statements that re-enter the engine stay checked", "[security][privileges]") {
    SecFixture f;
    CHECK_FALSE(runLast(f.ravi, "SET @x = (DIKHAO id SE hidden);").error.empty());
    runLast(f.ravi, "PREPARE p FROM 'DIKHAO ?';");
    CHECK_FALSE(runLast(f.ravi, "EXECUTE p USING (DIKHAO id SE hidden);").error.empty());
    CHECK_FALSE(runLast(f.ravi, "PREPARE q FROM (DIKHAO 'DIKHAO 1' SE hidden);").error.empty());
    f.admin.execute("BANAO FUNCTION peek() RETURNS INT SHURU RETURN (DIKHAO id SE hidden); KHATAM;");
    CHECK_FALSE(runLast(f.ravi, "DIKHAO peek();").error.empty());
}

TEST_CASE("security EXPLAIN of a set operation needs DIKHAO on both sides", "[security][privileges]") {
    SecFixture f;
    CHECK(runLast(f.ravi, "SAMJHAO DIKHAO id SE hidden SANYUKT DIKHAO id SE hidden;").error ==
          "[Execution Galti] 'ravi' ko table 'hidden' par DIKHAO ka adhikar nahi hai");
    f.admin.execute("ADHIKAR DO DIKHAO PAR hidden KO ravi;");
    CHECK(runLast(f.ravi, "SAMJHAO DIKHAO id SE hidden SANYUKT DIKHAO id SE hidden;").error.empty());
}
