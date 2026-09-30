// cpp/include/meradb/backend.h
//
// "Something you can run MeraDB statements against": an embedded engine
// (LocalBackend) or a connection to a server (Connection, client.h). Python
// gets this by duck typing (Engine and Connection have the same methods); C++
// needs an explicit interface. The command-line tool -- and, later, the shell
// and the workbench -- only ever talk to a Backend.
#pragma once
#include "meradb/engine.h"
#include <memory>
#include <string>
#include <vector>

namespace meradb {

class Backend {
public:
    virtual ~Backend() = default;

    // Never throws a MeraDBError for a bad statement: it gets Result.error.
    virtual std::vector<Result> runScript(const std::string& text) = 0;
    // Like runScript but throws the first error (Python's Engine.execute / Connection.execute).
    virtual std::vector<Result> execute(const std::string& text) = 0;
    virtual std::string currentDb() = 0;
    virtual bool inTransaction() = 0;
    virtual nlohmann::ordered_json schemaTree() = 0;
    // "local (<data folder>)" or "<host>:<port>".
    virtual std::string description() = 0;
    virtual void close() = 0;
};

// The engine runs inside this process and opens the data folder directly (like SQLite).
class LocalBackend : public Backend {
public:
    explicit LocalBackend(const std::string& dataDir) : engine_(dataDir) {}
    explicit LocalBackend(std::shared_ptr<Instance> instance) : engine_(std::move(instance)) {}

    Engine& engine() { return engine_; }

    std::vector<Result> runScript(const std::string& text) override { return engine_.runScript(text); }
    std::vector<Result> execute(const std::string& text) override { return engine_.execute(text); }
    std::string currentDb() override { return engine_.currentDb; }
    bool inTransaction() override { return engine_.inTransaction(); }
    nlohmann::ordered_json schemaTree() override { return engine_.schemaTree(); }
    std::string description() override { return "local (" + engine_.instance().dataDir() + ")"; }
    void close() override { engine_.close(); }

private:
    Engine engine_;
};

}  // namespace meradb
