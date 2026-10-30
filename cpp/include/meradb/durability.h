// cpp/include/meradb/durability.h
//
// WHEN DOES A WRITE REACH THE DISK? An acknowledged statement or PAKKA must survive a power cut, so its writes are
// forced out with syncFile() before the acknowledgement, but only once, at the end: HeapFile and Catalog report what they
// wrote to the Durability of the running statement, and that decides when to sync.
//
//   outside a transaction   the statement's files are synced when the statement ends
//   inside a transaction    nothing is synced until PAKKA: the snapshot in .wapas is what protects the old state
//   no statement running    (HeapFile or Catalog used directly) every write is synced at once
//
// The order is the whole point, see docs/ARCHITECTURE.md "Durability".
#pragma once
#include <set>
#include <string>

namespace meradb {

// MERADB_SYNC=relaxed: nothing is forced to the disk. A killed process still recovers correctly (the operating system has
// every write), but a power cut can lose acknowledged work and can leave a table file that does not load.
// Anything else, or not set: full durability.
bool syncRelaxed();

// Tests only: MERADB_TEST_CRASH_POINT=name[:n] ends the process on the spot (exit code 77) the n-th time it gets to that
// step, so crash_kill.py can hit each step of the protocol exactly.
void crashPoint(const char* name);

class Durability {
public:
    bool inTransaction() const { return inTransaction_; }
    void enterTransaction() { inTransaction_ = true; }
    void leaveTransaction() { inTransaction_ = false; }

    void markFile(const std::string& path) { dirtyFiles_.insert(path); }
    void markFolder(const std::string& path) { dirtyFolders_.insert(path); }
    void markTree(const std::string& folder) { dirtyTrees_.insert(folder); }
    // Force everything marked to the disk; StorageError if it could not be done. The marks are gone either way.
    void syncDirty();
    void forgetDirty();

private:
    bool inTransaction_ = false;
    std::set<std::string> dirtyFiles_, dirtyFolders_, dirtyTrees_;
};

// The outermost statement on this thread, for as long as it runs. Nested statements (a procedure's body, a trigger's) join it.
class StatementScope {
public:
    explicit StatementScope(Durability& durability);
    ~StatementScope();
    StatementScope(const StatementScope&) = delete;
    StatementScope& operator=(const StatementScope&) = delete;
    // The statement is done: make it durable. Call before acknowledging it. A statement that ended in an error may have
    // changed files too, and the destructor syncs those without throwing over the error.
    void finish();

private:
    Durability& durability_;
    Durability* previous_;
    bool outermost_;
    bool finished_ = false;
};

// What HeapFile and Catalog call. All paths are UTF-8.
namespace durable {
void wrote(const std::string& path);                    // bytes written into this file
void syncBeforeRename(const std::string& tmp);          // a finished temp file is about to be renamed over the real one
void renamed(const std::string& path);                  // a rename inside the file's folder has just happened
}  // namespace durable

}  // namespace meradb
