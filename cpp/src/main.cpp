// meradb_cli: the `meradb` command (see cli.h).
#include "meradb/cli.h"
#include "meradb/sys_compat.h"
#ifdef MERADB_HAVE_WORKBENCH
#include "meradb/workbench.h"
#endif

int main(int argc, char** argv) {
    meradb::sys::Utf8Console console;  // UTF-8 on a Windows console; untouched when piped
#ifdef MERADB_HAVE_WORKBENCH
    meradb::setWorkbenchRunner(&meradb::wb::runWorkbench);
#endif
    return meradb::cliMain(meradb::sys::commandLineArgs(argc, argv));
}
