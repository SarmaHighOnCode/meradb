// meradb_cli: the `meradb` command (see cli.h).
#include "meradb/cli.h"
#include "meradb/sys_compat.h"

int main(int argc, char** argv) { return meradb::cliMain(meradb::sys::commandLineArgs(argc, argv)); }
