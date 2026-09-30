// cpp/tests/test_engine_triggers.cpp
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"
#include "meradb/engine.h"
#include "test_util.h"
#include <thread>

using namespace meradb;
using meradb_test::TempDir;
using meradb_test::runLast;

namespace {

std::string cell(const Result& r, size_t row, size_t col) { return formatValue(r.rows.at(row).at(col)); }

struct Db {
    TempDir dir;
    Engine e{dir.str()};
    Db() {
        e.execute("BANAO TABLE accounts (id INT MUKHYA KUNJI, balance INT);"
                  "BANAO TABLE audit (id INT, note TEXT);");
    }
    Result q(const std::string& sql) { return runLast(e, sql); }
    std::vector<std::string> audit() {  // "id:note" per row, heap order
        std::vector<std::string> out;
        Result r = q("DIKHAO id, note SE audit;");
        for (size_t i = 0; i < r.rows.size(); ++i) out.push_back(cell(r, i, 0) + ":" + cell(r, i, 1));
        return out;
    }
};

}  // namespace

TEST_CASE("engine_triggers create and drop use Python's messages", "[engine][triggers]") {
    Db d;
    CHECK(d.q("BANAO TRIGGER t1 BAAD DAALO PAR accounts SHURU DIKHAO * SE t; KHATAM;").message ==
          "Trigger 't1' ban gaya (BAAD DAALO PAR accounts)");
    CHECK(d.q("BANAO TRIGGER t1 BAAD DAALO PAR accounts SHURU DIKHAO * SE t; KHATAM;").error ==
          "[Execution Galti] Trigger 't1' pehle se hai");
    CHECK(d.q("BANAO TRIGGER t2 BAAD DAALO PAR ghost SHURU DIKHAO * SE t; KHATAM;").error ==
          "[Execution Galti] Table 'ghost' exist nahi karta -- trigger sirf ek REAL table par lag sakta hai");
    d.q("BANAO VIEW av KAHO DIKHAO * SE accounts;");
    CHECK(d.q("BANAO TRIGGER t3 BAAD DAALO PAR av SHURU DIKHAO * SE t; KHATAM;").error ==
          "[Execution Galti] Table 'av' exist nahi karta -- trigger sirf ek REAL table par lag sakta hai");
    CHECK(d.q("HATAO TRIGGER t1;").message == "Trigger 't1' hata diya");
    CHECK(d.q("HATAO TRIGGER t1;").error == "[Execution Galti] Trigger 't1' exist nahi karta");
}

TEST_CASE("engine_triggers AFTER INSERT fires once per row with NAYA values", "[engine][triggers]") {
    Db d;
    d.q("BANAO TRIGGER t BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'inserted'); KHATAM;");
    CHECK(d.q("DAALO MEIN accounts MAAN (1, 100), (2, 50);").message == "2 row(s) daal di");
    CHECK(d.audit() == std::vector<std::string>{"1:inserted", "2:inserted"});
}

TEST_CASE("engine_triggers UPDATE sees NAYA and PURANA, DELETE sees PURANA", "[engine][triggers]") {
    Db d;
    d.q("DAALO MEIN accounts MAAN (1, 100), (2, 50);");
    d.q("BANAO TRIGGER u BAAD BADLO PAR accounts SHURU DAALO MEIN audit MAAN (PURANA.balance, 'was'); "
        "DAALO MEIN audit MAAN (NAYA.balance, 'now'); KHATAM;");
    d.q("BANAO TRIGGER x BAAD MITAO PAR accounts SHURU DAALO MEIN audit MAAN (PURANA.id, 'gone'); KHATAM;");
    CHECK(d.q("BADLO accounts RAKHO balance = balance + 5 JAHAN id = 1;").message == "1 row(s) badal di");
    CHECK(d.q("MITAO SE accounts JAHAN id = 2;").message == "1 row(s) mita di");
    CHECK(d.audit() == std::vector<std::string>{"100:was", "105:now", "2:gone"});
}

TEST_CASE("engine_triggers fire in creation order, per row", "[engine][triggers]") {
    Db d;
    d.q("BANAO TRIGGER a BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'first'); KHATAM;");
    d.q("BANAO TRIGGER b BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'second'); KHATAM;");
    d.q("DAALO MEIN accounts MAAN (1, 0), (2, 0);");
    CHECK(d.audit() == std::vector<std::string>{"1:first", "1:second", "2:first", "2:second"});
}

TEST_CASE("engine_triggers BEFORE trigger error vetoes the whole statement", "[engine][triggers]") {
    Db d;
    d.q("BANAO TRIGGER veto PEHLE DAALO PAR accounts SHURU MITAO SE nosuch; KHATAM;");
    CHECK(d.q("DAALO MEIN accounts MAAN (1, 1), (2, 2);").error == "[Execution Galti] Table 'nosuch' exist nahi karta");
    CHECK(d.q("DIKHAO * SE accounts;").rows.empty());
}

TEST_CASE("engine_triggers AFTER trigger error does not undo written rows", "[engine][triggers]") {
    Db d;
    d.q("BANAO TRIGGER bad BAAD DAALO PAR accounts SHURU MITAO SE nosuch; KHATAM;");
    CHECK(d.q("DAALO MEIN accounts MAAN (1, 1), (2, 2);").error == "[Execution Galti] Table 'nosuch' exist nahi karta");
    CHECK(d.q("DIKHAO * SE accounts;").rows.size() == 2);  // known limitation, mirrored from Python
}

TEST_CASE("engine_triggers upsert collisions fire UPDATE triggers, fresh rows fire INSERT triggers", "[engine][triggers]") {
    Db d;
    d.q("DAALO MEIN accounts MAAN (1, 10);");
    d.q("BANAO TRIGGER i BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'ins'); KHATAM;");
    d.q("BANAO TRIGGER u BAAD BADLO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'upd'); KHATAM;");
    CHECK(d.q("DAALO MEIN accounts MAAN (1, 99), (2, 5) TAKRAAV PAR BADLO balance = balance;").message ==
          "1 row(s) daali, 1 row(s) TAKRAAV par badli");
    // Python order: the updated (colliding) rows finish first, then the fresh rows are inserted
    CHECK(d.audit() == std::vector<std::string>{"1:upd", "2:ins"});
}

TEST_CASE("engine_triggers a trigger writing to its own table shares the index", "[engine][triggers]") {
    Db d;
    d.q("DAALO MEIN accounts MAAN (1, 10);");
    d.q("BANAO TRIGGER mirror BAAD BADLO PAR accounts SHURU DAALO MEIN accounts MAAN (NAYA.id + 100, 0); KHATAM;");
    CHECK(d.q("BADLO accounts RAKHO balance = 11 JAHAN id = 1;").error.empty());
    CHECK(d.q("DIKHAO id SE accounts;").rows.size() == 2);   // 1 and 101
    // the second update tries to insert 101 again: the shared unique index must notice
    CHECK_FALSE(d.q("BADLO accounts RAKHO balance = 12 JAHAN id = 1;").error.empty());
    CHECK(d.q("DIKHAO id SE accounts;").rows.size() == 2);
}

TEST_CASE("engine_triggers survive a restart and belong to their database", "[engine][triggers]") {
    TempDir dir;
    {
        Engine e(dir.str());
        e.execute("BANAO TABLE t (id INT); BANAO TABLE log (id INT);"
                  "BANAO TRIGGER tr BAAD DAALO PAR t SHURU DAALO MEIN log MAAN (NAYA.id); KHATAM;");
    }
    Engine e(dir.str());
    e.execute("DAALO MEIN t MAAN (7);");
    CHECK(cell(runLast(e, "DIKHAO id SE log;"), 0, 0) == "7");
    e.execute("BANAO DATABASE other; ISTEMAL other; BANAO TABLE t (id INT); BANAO TABLE log (id INT);");
    e.execute("DAALO MEIN t MAAN (8);");                      // no trigger in `other`
    CHECK(runLast(e, "DIKHAO id SE log;").rows.empty());
}

TEST_CASE("engine_triggers run with the invoker's privileges", "[engine][triggers]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    Engine admin(inst);
    Engine ravi(inst);
    ravi.user = "ravi";
    admin.execute("BANAO USER ravi GUPT 'pw'; BANAO TABLE t (id INT); BANAO TABLE log (id INT);"
                  "BANAO TRIGGER tr BAAD DAALO PAR t SHURU DAALO MEIN log MAAN (NAYA.id); KHATAM;"
                  "ADHIKAR DO DAALO PAR t KO ravi;");
    CHECK(runLast(ravi, "DAALO MEIN t MAAN (1);").error ==
          "[Execution Galti] 'ravi' ko table 'log' par DAALO ka adhikar nahi hai");
    admin.execute("ADHIKAR DO DAALO PAR log KO ravi;");
    CHECK(runLast(ravi, "DAALO MEIN t MAAN (2);").error.empty());
}

TEST_CASE("engine_triggers a self-triggering trigger hits the nesting cap, not the stack", "[engine][triggers]") {
    Db d;
    d.q("DAALO MEIN accounts MAAN (1, 10);");
    d.q("BANAO TRIGGER loop PEHLE BADLO PAR accounts SHURU BADLO accounts RAKHO balance = balance + 1; KHATAM;");
    // run on a plain std::thread: the server's connection threads have the default stack size
    std::string error;
    std::thread worker([&] { error = d.q("BADLO accounts RAKHO balance = 0;").error; });
    worker.join();
    CHECK(error.find("bahut gehra") != std::string::npos);
    CHECK(cell(d.q("DIKHAO balance SE accounts;"), 0, 0) == "10");   // a PEHLE trigger runs before any write
}

TEST_CASE("engine_triggers rollback undoes trigger side effects", "[engine][triggers]") {
    Db d;
    d.q("BANAO TRIGGER t BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'ins'); KHATAM;");
    d.q("SHURU;");
    d.q("DAALO MEIN accounts MAAN (1, 1);");
    CHECK(d.audit().size() == 1);
    d.q("WAPAS;");
    CHECK(d.audit().empty());
    CHECK(d.q("DIKHAO * SE accounts;").rows.empty());
}

TEST_CASE("engine_triggers golden scripts match the Python engine", "[engine][triggers][golden]") {
    meradb_test::replayGolden("trigger");
}
