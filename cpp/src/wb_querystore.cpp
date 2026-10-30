// cpp/src/wb_querystore.cpp -- see include/meradb/wb_querystore.h
#include "meradb/wb_querystore.h"
#include <cctype>
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <fstream>

namespace meradb::wb {

namespace fs = std::filesystem;

static std::string safeName(const std::string& db) {
    std::string out;
    for (unsigned char ch : db) out += (ch == '/' || ch == '\\' || ch == ':' || ch < 32) ? '_' : static_cast<char>(ch);
    return out.empty() ? "_" : out;
}

std::string QueryStore::fileFor(const std::string& db) const {
    return (fs::u8path(dataDir_) / ".history" / fs::u8path(safeName(db) + ".jsonl")).u8string();
}

std::string QueryStore::scriptFile(const std::string& db) const {
    return (fs::u8path(dataDir_) / ".history" / fs::u8path(safeName(db) + ".script.mdb")).u8string();
}

std::string QueryStore::nowStamp() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::vector<SavedQuery> QueryStore::load(const std::string& db) const {
    std::vector<SavedQuery> out;
    if (!enabled()) return out;
    try {
        std::ifstream in(fs::u8path(fileFor(db)), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto j = nlohmann::json::parse(line, nullptr, false);
            if (!j.is_object() || !j.contains("q") || !j["q"].is_string()) continue;
            SavedQuery q;
            q.text = j["q"].get<std::string>();
            if (j.contains("t") && j["t"].is_string()) q.when = j["t"].get<std::string>();
            out.push_back(std::move(q));
        }
    } catch (...) {
    }
    return out;
}

// BANAO USER naam GUPT '...' (or a typo of it, or PREPARE's quoted text): the password must not sit in a plain file.
// USER alone is not enough, a column can be called that.
static bool holdsPassword(const std::string& text) {
    std::string word;
    for (const char c : text + " ") {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            word += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else {
            if (word == "GUPT" || word == "IDENTIFIED") return true;
            word.clear();
        }
    }
    return false;
}

void QueryStore::append(const std::string& db, const SavedQuery& query) const {
    if (!enabled() || holdsPassword(query.text)) return;
    try {
        fs::create_directories(fs::u8path(dataDir_) / ".history");
        nlohmann::json j = {{"q", query.text}, {"t", query.when}};
        std::ofstream out(fs::u8path(fileFor(db)), std::ios::binary | std::ios::app);
        out << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    } catch (...) {
    }
}

std::string QueryStore::loadScript(const std::string& db) const {
    if (!enabled()) return "";
    try {
        std::ifstream in(fs::u8path(scriptFile(db)), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } catch (...) {
        return "";
    }
}

void QueryStore::saveScript(const std::string& db, const std::string& text) const {
    if (!enabled()) return;
    try {
        fs::create_directories(fs::u8path(dataDir_) / ".history");
        std::ofstream out(fs::u8path(scriptFile(db)), std::ios::binary | std::ios::trunc);
        out << text;
    } catch (...) {
    }
}

void QueryStore::clear(const std::string& db) const {
    if (!enabled()) return;
    try {
        std::error_code ec;
        fs::remove(fs::u8path(fileFor(db)), ec);
    } catch (...) {
    }
}

}  // namespace meradb::wb
