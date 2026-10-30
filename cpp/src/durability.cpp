// cpp/src/durability.cpp -- see include/meradb/durability.h
#include "meradb/durability.h"
#include "meradb/errors.h"
#include "meradb/fs_util.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace meradb {

namespace {

thread_local Durability* currentStatement = nullptr;

void syncOrThrow(const std::string& path) {
    if (!syncFile(path)) throw StorageError(path + " disk par pakki nahi hui");
}

// ---- the journal file: a header, then records. Only records carrying the header's number count, so what an earlier
// statement left further down the file is ignored, and so is a record a crash cut short (its checksum fails, and no
// write it described has happened: each write waits for its record to be on the disk).
//   header   "MDBUNDO1" number(8)
//   record   length(4) crc32(4) of number + payload, payload
constexpr char kLogMagic[] = "MDBUNDO1";
constexpr size_t kLogMagicLen = 8;
constexpr size_t kLogHeader = 16;
constexpr char kJournalFolder[] = "journal.tmp";  // ".tmp": a tool that does not know the journal just deletes it
constexpr uint8_t kOpSize = 'S', kOpTombstones = 'T', kOpImage = 'W', kOpAbsent = 'N', kOpRename = 'R';

uint32_t crc32Of(const uint8_t* bytes, size_t size, uint32_t crc = 0xFFFFFFFFu) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putText(std::vector<uint8_t>& out, const std::string& text) {
    putU32(out, static_cast<uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

// Reads a record's fields; `ok` goes false when it runs off the end.
struct Reader {
    const uint8_t* at;
    const uint8_t* end;
    bool ok = true;
    uint64_t number(int bytes) {
        if (end - at < bytes) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | at[i];
        at += bytes;
        return v;
    }
    std::string text() {
        const auto size = static_cast<size_t>(number(4));
        if (!ok || static_cast<size_t>(end - at) < size) {
            ok = false;
            return {};
        }
        std::string out(reinterpret_cast<const char*>(at), size);
        at += size;
        return out;
    }
};

uint32_t recordCrc(uint64_t sequence, const uint8_t* payload, size_t size) {
    std::vector<uint8_t> number;
    putU64(number, sequence);
    return ~crc32Of(payload, size, crc32Of(number.data(), number.size()));
}

// A plain file handle: the journal must write at an offset, sync, and never buffer.
#ifdef _WIN32
void* openLog(const std::string& path) {
    HANDLE file = CreateFileW(pathOf(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return file == INVALID_HANDLE_VALUE ? nullptr : file;
}
bool writeLog(void* file, uint64_t offset, const void* bytes, size_t size) {
    OVERLAPPED where{};
    where.Offset = static_cast<DWORD>(offset);
    where.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD written = 0;
    return WriteFile(static_cast<HANDLE>(file), bytes, static_cast<DWORD>(size), &written, &where) != 0 && written == size;
}
bool flushLog(void* file) { return FlushFileBuffers(static_cast<HANDLE>(file)) != 0; }
void closeLogFile(void* file) { CloseHandle(static_cast<HANDLE>(file)); }
#else
void* openLog(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    return fd < 0 ? nullptr : reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1);
}
int fdOf(void* file) { return static_cast<int>(reinterpret_cast<intptr_t>(file) - 1); }
bool writeLog(void* file, uint64_t offset, const void* bytes, size_t size) {
    return ::pwrite(fdOf(file), bytes, size, static_cast<off_t>(offset)) == static_cast<ssize_t>(size);
}
bool flushLog(void* file) { return ::fsync(fdOf(file)) == 0; }
void closeLogFile(void* file) { ::close(fdOf(file)); }
#endif

fs::path journalFolder(const std::string& dataDir) { return pathOf(dataDir) / ".wapas" / kJournalFolder; }

// Undoes one record. Each case gives the same result when repeated, so a crash during recovery just runs it again.
void undoRecord(const fs::path& data, const fs::path& journal, const uint8_t* payload, size_t size) {
    Reader in{payload, payload + size};
    const auto op = static_cast<uint8_t>(in.number(1));
    const std::string first = in.text();
    if (!in.ok) return;
    // A checksummed record is ours, but the names still come from a file: nothing outside the data folder is touched.
    const auto isInside = [](const std::string& name) {
        const fs::path path = pathOf(name);
        if (path.is_absolute() || path.has_root_name()) return false;
        return std::none_of(path.begin(), path.end(), [](const fs::path& part) { return part == ".."; });
    };
    if (!isInside(first)) throw StorageError("Journal corrupt hai: '" + first + "' data folder ke bahar hai");
    const fs::path target = data / pathOf(first);
    std::error_code ec;
    switch (op) {
        case kOpSize: {
            const uint64_t oldSize = in.number(8);
            const auto now = fs::file_size(target, ec);
            if (in.ok && !ec && now > oldSize) {
                fs::resize_file(target, oldSize, ec);
                if (ec) throw StorageError(first + " wapas chhoti nahi hui: " + ec.message());
                syncFile(textOf(target));
            }
            break;
        }
        case kOpTombstones: {
            std::fstream f(target, std::ios::binary | std::ios::in | std::ios::out);
            if (!f) break;  // the file is gone: a later step of this statement dropped it, and an earlier one brings it back
            f.seekg(0, std::ios::end);
            const auto fileSize = static_cast<uint64_t>(f.tellg());
            const uint64_t count = in.number(8);
            const char live = 1;
            for (uint64_t i = 0; in.ok && i < count; ++i) {
                const uint64_t offset = in.number(8);
                if (!in.ok || offset >= fileSize) continue;
                f.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
                f.write(&live, 1);
            }
            f.close();
            syncFile(textOf(target));
            break;
        }
        case kOpImage: {
            const std::string name = in.text();
            if (!in.ok || !isInside(name)) break;
            const fs::path image = journal / pathOf(name);
            if (!fs::exists(image, ec) || !fs::is_directory(target.parent_path(), ec)) break;
            // copied, not moved: if this recovery dies too, the next one needs the image again
            const fs::path staged = pathOf(textOf(target) + ".undo");
            fs::copy_file(image, staged, fs::copy_options::overwrite_existing, ec);
            if (ec) throw StorageError(first + " wapas nahi aayi: " + ec.message());
            syncFile(textOf(staged));
            fs::rename(staged, target, ec);
            if (ec) throw StorageError(first + " wapas nahi aayi: " + ec.message());
            syncFolderOf(textOf(target));
            break;
        }
        case kOpAbsent:
            fs::remove(target, ec);
            syncFolderOf(textOf(target));
            break;
        case kOpRename: {
            const std::string name = in.text();
            if (!in.ok || !isInside(name)) break;
            const fs::path newName = data / pathOf(name);
            if (fs::exists(newName, ec) && !fs::exists(target, ec)) {
                fs::rename(newName, target, ec);
                if (ec) throw StorageError(first + " ka naam wapas nahi aaya: " + ec.message());
                syncFolderOf(textOf(target));
            }
            break;
        }
        default: break;
    }
}

}  // namespace

bool syncRelaxed() {
    static const bool relaxed = [] {
        const char* value = std::getenv("MERADB_SYNC");
        return value != nullptr && std::string(value) == "relaxed";
    }();
    return relaxed;
}

namespace {
const char* const g_crashWanted = std::getenv("MERADB_TEST_CRASH_POINT");
}

bool crashPointArmed(const char* name) {
    if (g_crashWanted == nullptr) return false;
    const std::string spec(g_crashWanted);
    return spec.substr(0, spec.find(':')) == name;
}

void crashPoint(const char* name) {
    if (g_crashWanted == nullptr) return;
    static const std::string spec(g_crashWanted);
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

// ---- the journal of the running statement

Durability::~Durability() {
    closeLog();
    if (sequence_ != 0) {  // this Instance wrote one: a clean close leaves .wapas as it found it
        std::error_code ec;
        fs::remove_all(journalFolder(dataDir_), ec);
    }
}

std::string Durability::relative(const std::string& path) const {
    return pathOf(path).lexically_relative(pathOf(dataDir_)).generic_u8string();
}

void Durability::closeLog() {
    if (logFile_ != nullptr) closeLogFile(logFile_);
    logFile_ = nullptr;
}

void Durability::appendRecord(const std::vector<uint8_t>& payload) {
    const bool relaxed = syncRelaxed();
    std::vector<uint8_t> bytes;
    if (logEnd_ == 0) {  // the statement's first record: a new number, in the same write as the record
        if (logFile_ == nullptr) {
            const fs::path folder = journalFolder(dataDir_);
            std::error_code ec;
            const bool fresh = !fs::exists(folder / "log", ec);
            fs::create_directories(folder, ec);
            logFile_ = openLog(textOf(folder / "log"));
            if (logFile_ == nullptr) throw StorageError(textOf(folder / "log") + " khul nahi paayi");
            if (fresh && !relaxed) {
                syncFolder(textOf(folder));
                syncFolder(textOf(folder.parent_path()));
                syncFolder(dataDir_);
            }
        }
        // the clock keeps numbers apart across restarts; the +1 within one microsecond
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        sequence_ = std::max(sequence_ + 1, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count()));
        bytes.insert(bytes.end(), kLogMagic, kLogMagic + kLogMagicLen);
        putU64(bytes, sequence_);
    }
    putU32(bytes, static_cast<uint32_t>(payload.size()));
    putU32(bytes, recordCrc(sequence_, payload.data(), payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if (!writeLog(logFile_, logEnd_, bytes.data(), bytes.size()) || (!relaxed && !flushLog(logFile_)))
        throw StorageError("Journal disk par nahi likh paaye");
    logEnd_ += bytes.size();
    crashPoint("stmt-after-log");
}

void Durability::beforeAppend(const std::string& path) {
    if (inTransaction_ || sizeLogged_.count(path)) return;  // the earliest size is the one that counts
    std::error_code ec;
    const auto size = fs::file_size(pathOf(path), ec);
    std::vector<uint8_t> payload{ec ? kOpAbsent : kOpSize};
    putText(payload, relative(path));
    if (!ec) putU64(payload, size);
    appendRecord(payload);
    sizeLogged_[path] = ec ? 0 : size;
}

void Durability::beforeTombstones(const std::string& path, const std::vector<int64_t>& offsets) {
    if (inTransaction_) return;
    const auto logged = sizeLogged_.find(path);
    std::vector<int64_t> needed;  // a row this statement added goes with the truncate anyway
    for (auto offset : offsets)
        if (logged == sizeLogged_.end() || static_cast<uint64_t>(offset) < logged->second) needed.push_back(offset);
    if (needed.empty()) return;
    std::vector<uint8_t> payload{kOpTombstones};
    putText(payload, relative(path));
    putU64(payload, needed.size());
    for (auto offset : needed) putU64(payload, static_cast<uint64_t>(offset));
    appendRecord(payload);
}

void Durability::beforeReplace(const std::string& path) {
    if (inTransaction_) return;
    sizeLogged_.erase(path);  // the next append works on a different file
    std::error_code ec;
    if (!fs::exists(pathOf(path), ec)) {
        std::vector<uint8_t> payload{kOpAbsent};
        putText(payload, relative(path));
        appendRecord(payload);
        return;
    }
    const fs::path folder = journalFolder(dataDir_);
    fs::create_directories(folder, ec);
    const std::string name = "image" + std::to_string(++imageCount_);
    const fs::path image = folder / name;
    fs::remove(image, ec);
    // A hard link costs nothing whatever the size of the file: the rename coming next leaves the old bytes alone, under
    // this name. Copied where the file system has no hard links.
    fs::create_hard_link(pathOf(path), image, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(pathOf(path), image, ec);
        if (ec) throw StorageError(path + " ki copy nahi ban paayi: " + ec.message());
    }
    if (!syncRelaxed()) {
        if (!syncFile(textOf(image))) throw StorageError(textOf(image) + " disk par pakki nahi hui");
        syncFolder(textOf(folder));
    }
    std::vector<uint8_t> payload{kOpImage};
    putText(payload, relative(path));
    putText(payload, name);
    appendRecord(payload);
}

void Durability::beforeRename(const std::string& from, const std::string& to) {
    if (inTransaction_) return;
    sizeLogged_.erase(from);
    std::vector<uint8_t> payload{kOpRename};
    putText(payload, relative(from));
    putText(payload, relative(to));
    appendRecord(payload);
}

void Durability::finishStatement() {
    const bool logged = logEnd_ != 0;
    logEnd_ = 0;
    sizeLogged_.clear();
    std::string failure;
    try {
        syncDirty();
    } catch (const StorageError& e) {
        failure = e.what();
    }
    crashPoint("stmt-after-sync");
    if (logged) {
        // <- the statement's COMMIT POINT. Also after a failed sync: the files already hold the statement, and the next
        // one must not start on top of a journal that would undo this one.
        const char wiped[kLogHeader] = {};
        const bool ok = writeLog(logFile_, 0, wiped, kLogHeader) && (syncRelaxed() || flushLog(logFile_));
        if (!ok && failure.empty()) failure = "Journal disk par nahi likh paaye";
        crashPoint("stmt-after-commit");
    }
    closeLog();
    if (imageCount_ > 0) {
        std::error_code ec;
        const fs::path folder = journalFolder(dataDir_);
        for (int i = 1; i <= imageCount_; ++i) fs::remove(folder / ("image" + std::to_string(i)), ec);
        imageCount_ = 0;
    }
    if (!failure.empty()) throw StorageError(failure);
}

std::vector<std::string> Durability::undoInterruptedStatement(const std::string& dataDir) {
    const fs::path journal = journalFolder(dataDir);
    const fs::path data = pathOf(dataDir);
    std::vector<std::string> touched;
    std::error_code ec;
    if (!fs::exists(journal, ec)) return touched;

    std::ifstream in(journal / "log", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (bytes.size() >= kLogHeader && std::memcmp(bytes.data(), kLogMagic, kLogMagicLen) == 0) {
        Reader header{bytes.data() + kLogMagicLen, bytes.data() + kLogHeader};
        const uint64_t sequence = header.number(8);
        std::vector<std::pair<size_t, size_t>> records;  // offset, size
        size_t at = kLogHeader;
        while (bytes.size() - at >= 8) {
            Reader head{bytes.data() + at, bytes.data() + at + 8};
            const auto size = static_cast<size_t>(head.number(4));
            const auto crc = static_cast<uint32_t>(head.number(4));
            if (bytes.size() - at - 8 < size || recordCrc(sequence, bytes.data() + at + 8, size) != crc) break;
            records.emplace_back(at + 8, size);
            at += 8 + size;
        }
        // newest first: each undo finds the file as that step left it
        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            undoRecord(data, journal, bytes.data() + it->first, it->second);
            crashPoint("recovery-after-undo");
            Reader name{bytes.data() + it->first + 1, bytes.data() + it->first + it->second};
            const std::string path = name.text();
            const std::string db = path.substr(0, path.find('/'));
            if (name.ok && std::find(touched.begin(), touched.end(), db) == touched.end()) touched.push_back(db);
        }
        std::sort(touched.begin(), touched.end());
        // The header goes first, so that a crash in the delete below cannot repeat the undo without its images.
        if (void* log = openLog(textOf(journal / "log"))) {
            const char wiped[kLogHeader] = {};
            writeLog(log, 0, wiped, kLogHeader);
            flushLog(log);
            closeLogFile(log);
        }
    }
    fs::remove_all(journal, ec);
    syncFolder(textOf(journal.parent_path()));
    return touched;
}

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
            durability_.finishStatement();
        } catch (const std::exception&) {  // the statement's own error is already on its way out
        }
    }
    currentStatement = previous_;
}

void StatementScope::finish() {
    finished_ = true;
    if (outermost_ && !durability_.inTransaction()) durability_.finishStatement();
}

// ---------------------------------------------------------------- what the storage layer calls

namespace durable {

void beforeAppend(const std::string& path) {
    if (currentStatement != nullptr) currentStatement->beforeAppend(path);
}
void beforeTombstones(const std::string& path, const std::vector<int64_t>& offsets) {
    if (currentStatement != nullptr) currentStatement->beforeTombstones(path, offsets);
}
void beforeReplace(const std::string& path) {
    if (currentStatement != nullptr) currentStatement->beforeReplace(path);
}
void beforeRename(const std::string& from, const std::string& to) {
    if (currentStatement != nullptr) currentStatement->beforeRename(from, to);
}

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
