// cpp/src/workbench.cpp (placeholder; a later task replaces the body)
#include "meradb/workbench.h"

namespace meradb::wb {

int runWorkbench(std::unique_ptr<Backend> backend, const CliArgs&) {
    if (backend) backend->close();
    return 0;
}

}  // namespace meradb::wb
