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
// A statement outside a transaction also has an UNDO JOURNAL (.wapas/journal.tmp/log), so that a crash half-way through
// it is undone at the next start. Each step is on the disk before the write it describes:
//
//   a table is about to grow            its old size
//   rows are about to get tombstones    their offsets
//   a file is about to be replaced      a hard link to the old file (a rename leaves that file's bytes alone)
//   a table is about to be renamed      the two names
//
// When the statement's files are synced the journal's header is wiped (and synced): THAT is the statement's commit point.
//
// The order is the whole point, see docs/ARCHITECTURE.md "Durability".
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace meradb {

// MERADB_SYNC=relaxed: nothing is forced to the disk. A killed process still recovers correctly (the operating system has
// every write), but a power cut can lose acknowledged work and can leave a table file that does not load.
// Anything else, or not set: full durability.
bool syncRelaxed();

// Tests only: MERADB_TEST_CRASH_POINT=name[:n] ends the process on the spot (exit code 77) the n-th time it gets to that
// step, so crash_kill.py can hit each step of the protocol exactly.
void crashPoint(const char* name);
bool crashPointArmed(const char* name);  // is this the point the variable names? (to set up a torn write first)

class Durability {
public:
    explicit Durability(std::string dataDir) : dataDir_(std::move(dataDir)) {}
    ~Durability();  // a clean close leaves no journal behind
    Durability(const Durability&) = delete;
    Durability& operator=(const Durability&) = delete;

    // Crash recovery, before anything else looks at the files: undoes the statement a crash interrupted, if any, and
    // returns the databases it touched. Running it again after a crash half-way through it gives the same result.
    static std::vector<std::string> undoInterruptedStatement(const std::string& dataDir);

    // What is about to happen to a file. Nothing to do in a transaction, where the snapshot covers everything.
    void beforeAppend(const std::string& path);
    void beforeTombstones(const std::string& path, const std::vector<int64_t>& offsets);
    void beforeReplace(const std::string& path);
    void beforeRename(const std::string& from, const std::string& to);

    // The statement is done: sync what it wrote, then wipe the journal. StorageError if the sync failed.
    void finishStatement();

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
    std::string dataDir_;
    bool inTransaction_ = false;
    std::set<std::string> dirtyFiles_, dirtyFolders_, dirtyTrees_;

    // the journal of the running statement
    uint64_t sequence_ = 0;
    uint64_t logEnd_ = 0;  // where the next record goes; 0 = nothing logged yet
    int imageCount_ = 0;
    void* logFile_ = nullptr;
    std::map<std::string, uint64_t> sizeLogged_;  // files whose old size is in the journal, and that size

    void appendRecord(const std::vector<uint8_t>& payload);
    void closeLog();
    std::string relative(const std::string& path) const;
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
void beforeAppend(const std::string& path);
void beforeTombstones(const std::string& path, const std::vector<int64_t>& offsets);
void beforeReplace(const std::string& path);  // it will be renamed over or removed, never changed in place
void beforeRename(const std::string& from, const std::string& to);
void wrote(const std::string& path);            // bytes written into this file
void syncBeforeRename(const std::string& tmp);  // a finished temp file is about to be renamed over the real one
void renamed(const std::string& path);          // a rename or a removal inside the file's folder has just happened
}  // namespace durable

}  // namespace meradb
