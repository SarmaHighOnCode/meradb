// cpp/include/meradb/wb_querystore.h
//
// The saved queries of the workbench: every statement run in a database is appended to
// <data folder>/.history/<database>.jsonl (one {"q": text, "t": "YYYY-MM-DD HH:MM:SS"} object per line), so the
// "Queries" list is still there the next time the program starts. The folder starts with a dot, so the engine does not
// list it as a database. Failures to read or write are silent: a history that cannot be saved must never stop a query.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace meradb::wb {

struct SavedQuery {
    std::string text;
    std::string when;   // local time, "YYYY-MM-DD HH:MM:SS"; "" when unknown
};

class QueryStore {
public:
    explicit QueryStore(std::string dataDir) : dataDir_(std::move(dataDir)) {}
    bool enabled() const { return !dataDir_.empty(); }
    std::vector<SavedQuery> load(const std::string& db) const;                // oldest first; damaged lines are skipped
    void append(const std::string& db, const SavedQuery& query) const;        // creates the folder on first use
    // The writing area's text of a database (<db>.script.mdb), so the script is still there at the next start.
    std::string loadScript(const std::string& db) const;
    void saveScript(const std::string& db, const std::string& text) const;
    void clear(const std::string& db) const;                                  // deletes that database's file
    std::string fileFor(const std::string& db) const;
    static std::string nowStamp();

private:
    std::string scriptFile(const std::string& db) const;
    std::string dataDir_;
};

}  // namespace meradb::wb
