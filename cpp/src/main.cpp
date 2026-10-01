// meradb_cli: the `meradb` command (see cli.h).
#include "meradb/cli.h"
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    return meradb::cliMain(std::move(args));
}
