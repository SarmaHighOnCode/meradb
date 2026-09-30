// cpp/tests/golden_runner.h -- replays the scripts of golden_engine.h (generated
// from the Python reference engine, see tests/golden/gen_golden.py) and
// compares every statement's outcome with what Python produced.
#pragma once
#include <catch2/catch_test_macros.hpp>
#include "golden_engine.h"
#include "meradb/engine.h"
#include "test_util.h"
#include <map>
#include <memory>
#include <string>

namespace meradb_test {

// Same canonical text gen_golden.py writes:  C:<columns> R:<rows> M:<message> E:<error>
inline std::string canonical(const meradb::Result& r) {
    std::string s = "C:";
    for (size_t i = 0; i < r.columns.size(); ++i) s += (i ? "," : "") + r.columns[i];
    s += " R:";
    for (size_t i = 0; i < r.rows.size(); ++i) {
        s += i ? ";" : "";
        for (size_t j = 0; j < r.rows[i].size(); ++j) s += (j ? "|" : "") + meradb::formatValue(r.rows[i][j]);
    }
    return s + " M:" + r.message + " E:" + r.error;
}

// Last result of a script (runScript never throws a MeraDBError).
inline meradb::Result runLast(meradb::Engine& e, const std::string& sql) {
    meradb::Result last;
    for (auto& r : e.runScript(sql)) last = r;
    return last;
}

// Replay every golden script of `group`. One Instance per script; `@name: sql` lines
// run on a restricted Engine for user `name` created on first use.
inline void replayGolden(const std::string& group) {
    int scriptsRun = 0;
    for (const auto& script : golden::scripts()) {
        if (group != script.group) continue;
        ++scriptsRun;
        TempDir dir;
        auto instance = std::make_shared<meradb::Instance>(dir.str());
        meradb::Engine admin(instance);
        std::map<std::string, std::unique_ptr<meradb::Engine>> sessions;
        int n = 0;
        for (const auto& step : script.steps) {
            ++n;
            INFO("script " << script.group << "_" << script.name << ", step " << n << ": " << step.sql);
            std::string sql = step.sql;
            meradb::Engine* target = &admin;
            if (!sql.empty() && sql[0] == '@') {
                size_t colon = sql.find(':');
                REQUIRE(colon != std::string::npos);
                std::string user = sql.substr(1, colon - 1);
                sql = sql.substr(colon + 1);
                sql.erase(0, sql.find_first_not_of(" \t"));
                auto& slot = sessions[user];
                if (!slot) {
                    slot = std::make_unique<meradb::Engine>(instance);
                    slot->user = user;
                }
                target = slot.get();
            }
            CHECK(canonical(runLast(*target, sql)) == step.expect);
        }
    }
    REQUIRE(scriptsRun > 0);
}

}  // namespace meradb_test
