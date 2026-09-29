// cpp/src/engine.cpp -- mirrors meradb/engine.py
#include "meradb/engine.h"
#include "meradb/errors.h"
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace meradb {

// ============================================================================
// Instance: shared state for one data folder
// ============================================================================

namespace {

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// shutil.rmtree(path, ignore_errors=True)
void removeTreeQuietly(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

// os.replace for a directory: atomic rename; any failure is a StorageError.
void replacePath(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) throw StorageError("'" + from.string() + "' ko '" + to.string() + "' nahi bana paaye: " + ec.message());
}

}  // namespace

Instance::Instance(std::string dataDir) : dataDir_(fs::absolute(fs::path(dataDir)).string()) {
    fs::create_directories(dataDir_);
    recovered_ = recover();
    fs::create_directories(dbDir(DEFAULT_DATABASE));
}

std::string Instance::dbDir(const std::string& name) const { return (fs::path(dataDir_) / name).string(); }

std::vector<std::string> Instance::databases() const {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dataDir_)) {
        std::string name = entry.path().filename().string();
        if (!name.empty() && name[0] != '.' && entry.is_directory()) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

Catalog& Instance::catalog(const std::string& db) {
    auto it = catalogs_.find(db);
    if (it == catalogs_.end()) it = catalogs_.emplace(db, std::make_unique<Catalog>(dbDir(db))).first;
    return *it->second;
}

void Instance::forget(const std::string& db) {
    catalogs_.erase(db);
    indexes_.erase(db);
}

IndexCache Instance::indexCache(const std::string& db, const std::string& table) {
    auto& slot = indexes_[db][table];
    if (!slot) slot = std::make_shared<std::optional<IndexMap>>();
    return slot;
}

void Instance::dropIndexCache(const std::string& db, const std::string& table) {
    auto it = indexes_.find(db);
    if (it == indexes_.end()) return;
    auto slot = it->second.find(table);
    if (slot == it->second.end()) return;
    slot->second->reset();  // any Table still holding the slot sees it emptied too
    it->second.erase(slot);
}

std::string Instance::snapshotPath(const std::string& db, const std::string& suffix) const {
    return (fs::path(dataDir_) / SNAPSHOT_DIR / (db + suffix)).string();
}

void Instance::takeSnapshot(const std::string& db) {
    fs::path tmp = snapshotPath(db, ".tmp");
    removeTreeQuietly(tmp);
    fs::create_directories(tmp.parent_path());
    std::error_code ec;
    fs::copy(dbDir(db), tmp, fs::copy_options::recursive, ec);
    if (ec) throw StorageError("Database '" + db + "' ki copy nahi ban paayi: " + ec.message());
    replacePath(tmp, snapshotPath(db));  // only a COMPLETE copy ever gets the real name
}

void Instance::discardSnapshot(const std::string& db) {
    fs::path done = snapshotPath(db, ".done");
    replacePath(snapshotPath(db), done);  // <- the COMMIT POINT: after this, no rollback
    removeTreeQuietly(done);
}

void Instance::restoreSnapshot(const std::string& db) {
    removeTreeQuietly(dbDir(db));
    replacePath(snapshotPath(db), dbDir(db));
    forget(db);
}

std::vector<std::string> Instance::recover() {
    // CRASH RECOVERY, run at startup. A snapshot still lying around means the
    // process died in the middle of a transaction that never reached PAKKA,
    // so it is rolled back. `.tmp` = SHURU never finished; `.done` = PAKKA
    // already happened -- both are just deleted.
    fs::path root = fs::path(dataDir_) / SNAPSHOT_DIR;
    std::vector<std::string> recovered;
    if (!fs::is_directory(root)) return recovered;
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(root)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        if (endsWith(name, ".tmp") || endsWith(name, ".done")) {
            removeTreeQuietly(root / name);
        } else {
            restoreSnapshot(name);
            recovered.push_back(name);
        }
    }
    return recovered;
}

// ============================================================================
// Engine: one session
// ============================================================================

Engine::Engine(std::shared_ptr<Instance> instance) : instance_(std::move(instance)) {}
Engine::Engine(const std::string& dataDir) : instance_(std::make_shared<Instance>(dataDir)) {}

Engine::~Engine() {
    // Like a Python Engine that is simply dropped (no close()): an open
    // transaction is NOT rolled back here -- its snapshot stays on disk and
    // crash recovery undoes it at the next start. Only the extra lock hold
    // taken by SHURU is released.
    if (inTransaction()) instance_->lock.unlock();
}

Catalog& Engine::catalog() { return instance_->catalog(currentDb); }

// ---------------------------------------------------------------- public API

std::vector<Result> Engine::execute(const std::string&) { throw ExecutionError("not implemented yet"); }
std::vector<Result> Engine::runScript(const std::string&) { throw ExecutionError("not implemented yet"); }
Result Engine::executeStatement(const ast::Statement&) { throw ExecutionError("not implemented yet"); }
void Engine::close() {}

}  // namespace meradb
