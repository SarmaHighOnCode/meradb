// cpp/src/durability.cpp -- see include/meradb/durability.h
#include "meradb/durability.h"
#include "meradb/errors.h"
#include "meradb/fs_util.h"
#include <cstdlib>
#include <filesystem>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace meradb {

namespace {

thread_local Durability* currentStatement = nullptr;

void syncOrThrow(const std::string& path) {
    if (!syncFile(path)) throw StorageError(path + " disk par pakki nahi hui");
}

}  // namespace

bool syncRelaxed() {
    static const bool relaxed = [] {
        const char* value = std::getenv("MERADB_SYNC");
        return value != nullptr && std::string(value) == "relaxed";
    }();
    return relaxed;
}

void crashPoint(const char* name) {
    static const char* const wanted = std::getenv("MERADB_TEST_CRASH_POINT");
    if (wanted == nullptr) return;
    static const std::string spec(wanted);
    static const auto colon = spec.find(':');
    static const std::string point = spec.substr(0, colon);
    static int remaining = colon == std::string::npos ? 1 : std::atoi(spec.c_str() + colon + 1);
    if (point != name || --remaining > 0) return;
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 77);
#else
    _exit(77);
#endif
}

// ---------------------------------------------------------------- Durability

void Durability::syncDirty() {
    std::set<std::string> files, folders, trees;
    files.swap(dirtyFiles_);
    folders.swap(dirtyFolders_);
    trees.swap(dirtyTrees_);
    if (syncRelaxed()) return;
    for (const auto& path : files) {
        std::error_code ec;
        if (fs::exists(pathOf(path), ec)) syncOrThrow(path);  // a file written and then dropped has nothing left to sync
    }
    for (const auto& tree : trees)
        if (!syncTree(tree)) throw StorageError(tree + " disk par pakki nahi hui");
    for (const auto& folder : folders) syncFolder(folder);
}

void Durability::forgetDirty() {
    dirtyFiles_.clear();
    dirtyFolders_.clear();
    dirtyTrees_.clear();
}

// ---------------------------------------------------------------- StatementScope

StatementScope::StatementScope(Durability& durability)
    : durability_(durability), previous_(currentStatement), outermost_(currentStatement != &durability) {
    currentStatement = &durability;
}

StatementScope::~StatementScope() {
    if (outermost_ && !finished_ && !durability_.inTransaction()) {
        try {
            durability_.syncDirty();
        } catch (const std::exception&) {  // the statement's own error is already on its way out
        }
    }
    currentStatement = previous_;
}

void StatementScope::finish() {
    finished_ = true;
    if (outermost_ && !durability_.inTransaction()) durability_.syncDirty();
}

// ---------------------------------------------------------------- what the storage layer calls

namespace durable {

void wrote(const std::string& path) {
    if (currentStatement != nullptr) currentStatement->markFile(path);
    else if (!syncRelaxed()) syncOrThrow(path);
}

void syncBeforeRename(const std::string& tmp) {
    // Inside a statement the file is synced at its end, and until then the old state is what recovery goes back to.
    if (currentStatement == nullptr && !syncRelaxed()) syncOrThrow(tmp);
}

void renamed(const std::string& path) {
    if (currentStatement != nullptr) {
        currentStatement->markFile(path);
        currentStatement->markFolder(textOf(pathOf(path).parent_path()));
    } else if (!syncRelaxed()) {
        syncFolderOf(path);
    }
}

}  // namespace durable

}  // namespace meradb
