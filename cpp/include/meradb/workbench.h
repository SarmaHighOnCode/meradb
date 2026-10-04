// cpp/include/meradb/workbench.h
//
// The full-screen workbench (mirrors meradb/tui.py's run_workbench). Only built when the CMake option
// MERADB_WORKBENCH is ON; cli.cpp reaches it through the hook set by main() (cli.h: setWorkbenchRunner), never
// directly, so meradb_core does not depend on FTXUI.
#pragma once
#include "meradb/backend.h"
#include "meradb/cli.h"
#include <memory>

namespace meradb::wb {

// Takes ownership of `backend`, runs the UI until the user quits, closes the backend (an unfinished
// transaction is rolled back) and returns the process exit code (0).
int runWorkbench(std::unique_ptr<Backend> backend, const CliArgs& args);

}  // namespace meradb::wb
