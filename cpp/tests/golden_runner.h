// cpp/tests/golden_runner.h -- replays the scripts of golden_engine.h (generated
// from the Python reference engine, see tests/golden/gen_golden.py) and
// compares every statement's outcome with what Python produced.
#pragma once
#include <catch2/catch_test_macros.hpp>
#include "golden_engine.h"
#include "meradb/engine.h"
#include "test_util.h"
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

// Replay every golden script of `group` on a fresh Engine each.
inline void replayGolden(const std::string& group) {
    int scriptsRun = 0;
    for (const auto& script : golden::scripts()) {
        if (group != script.group) continue;
        ++scriptsRun;
        TempDir dir;
        meradb::Engine e(dir.str());
        int n = 0;
        for (const auto& step : script.steps) {
            ++n;
            INFO("script " << script.group << "_" << script.name << ", step " << n << ": " << step.sql);
            CHECK(canonical(runLast(e, step.sql)) == step.expect);
        }
    }
    REQUIRE(scriptsRun > 0);
}

}  // namespace meradb_test
