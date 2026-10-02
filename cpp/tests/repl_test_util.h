// cpp/tests/repl_test_util.h -- a scriptable Backend for the shell tests.
#pragma once
#include "meradb/backend.h"
#include "meradb/errors.h"
#include <functional>
#include <string>
#include <vector>

namespace meradb_test {

class FakeBackend : public meradb::Backend {
public:
    std::vector<std::string> scripts;  // every text passed to runScript, in order
    // What runScript returns; the default is one message result "ok". May throw.
    std::function<std::vector<meradb::Result>(const std::string&)> onRun = [](const std::string&) {
        meradb::Result r;
        r.message = "ok";
        return std::vector<meradb::Result>{r};
    };
    std::string db = "main";
    bool txn = false;
    std::string where = "fake:1";
    bool closed = false;

    std::vector<meradb::Result> runScript(const std::string& text) override {
        scripts.push_back(text);
        return onRun(text);
    }
    std::vector<meradb::Result> execute(const std::string& text) override { return runScript(text); }
    std::string currentDb() override { return db; }
    bool inTransaction() override { return txn; }
    nlohmann::ordered_json schemaTree() override { return nlohmann::ordered_json::array(); }
    std::string description() override { return where; }
    void close() override { closed = true; }
};

}  // namespace meradb_test
