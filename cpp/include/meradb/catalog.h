// cpp/include/meradb/catalog.h
//
// The CATALOG: the database's "data about data". It remembers which tables
// (and views) exist and what their columns are; the rows themselves live in
// <table>.tbl heap files (see storage.h). Mirrors meradb/catalog.py and
// reads/writes the same catalog.json shape, so a database directory can be
// opened by either engine.
#pragma once
#include "meradb/datatypes.h"
#include <nlohmann/json.hpp>
#include <cstddef>
#include <iterator>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace meradb {

// A string-keyed map that iterates in INSERTION order, like a Python dict.
// catalog.json lists tables/views in creation order and several engine
// checks walk every table in that order, so the order must survive a
// save/reload round trip. Element references stay valid until that element
// is erased (backed by std::list).
template <typename V>
class InsertionOrderedMap {
public:
    using value_type = std::pair<const std::string, V>;
    using iterator = typename std::list<value_type>::iterator;
    using const_iterator = typename std::list<value_type>::const_iterator;

    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }

    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    size_t count(const std::string& key) const { return index_.count(key); }

    iterator find(const std::string& key) {
        auto it = index_.find(key);
        return it == index_.end() ? items_.end() : it->second;
    }
    const_iterator find(const std::string& key) const {
        auto it = index_.find(key);
        return it == index_.end() ? items_.cend() : const_iterator(it->second);
    }

    V& at(const std::string& key) { return index_.at(key)->second; }
    const V& at(const std::string& key) const { return index_.at(key)->second; }

    // Existing keys keep their position (Python dict semantics); new keys go last.
    V& operator[](const std::string& key) {
        auto it = index_.find(key);
        if (it != index_.end()) return it->second->second;
        items_.emplace_back(key, V{});
        auto last = std::prev(items_.end());
        index_.emplace(key, last);
        return last->second;
    }

    size_t erase(const std::string& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return 0;
        items_.erase(it->second);
        index_.erase(it);
        return 1;
    }

    void clear() {
        items_.clear();
        index_.clear();
    }

private:
    std::list<value_type> items_;
    std::unordered_map<std::string, iterator> index_;
};

struct Column {
    std::string name;
    std::string typeName;
    bool primaryKey = false;
    bool notNull = false;
    bool unique = false;
    std::optional<Value> defaultValue;  // WARNA value; nullopt (or a NULL Value) = no default
    std::optional<int> maxLength;       // VARCHAR(n)/CHAR(n)
    std::optional<std::string> refTable;   // SANDARBH parent table
    std::optional<std::string> refColumn;  // SANDARBH parent column
    std::optional<std::string> check;      // SHART source text

    bool isUnique() const { return primaryKey || unique; }
    bool isRequired() const { return primaryKey || notNull; }

    nlohmann::ordered_json toJson() const;
    // Reads the "default" key back and coerces it to typeName, so a DATE
    // default (stored as "YYYY-MM-DD") comes back as a Date, an INT as int64...
    static Column fromJson(const nlohmann::ordered_json& j);
};

struct TableSchema {
    std::string name;
    std::vector<Column> columns;
    // Table-level `ANOKHA (a, b)` / `MUKHYA KUNJI (a, b)` groups only; a plain
    // single-column constraint lives on the Column itself.
    std::vector<std::vector<std::string>> compositeUnique;
    std::optional<std::vector<std::string>> compositePk;

    std::vector<std::string> columnNames() const;
    std::vector<std::string> types() const;
    size_t indexOf(const std::string& column) const;  // throws ExecutionError if missing
    const Column& getColumn(const std::string& column) const;

    nlohmann::ordered_json toJson() const;
    static TableSchema fromJson(const nlohmann::ordered_json& j);
};

class Catalog {
public:
    static constexpr const char* kFileName = "catalog.json";

    explicit Catalog(std::string dbDir);

    InsertionOrderedMap<TableSchema> tables;
    InsertionOrderedMap<std::string> views;  // view name -> its DIKHAO source text
    // Phase 2 objects: not interpreted in Phase 1, but carried through
    // load/save untouched so a catalog written by the Python engine keeps them.
    nlohmann::ordered_json triggers = nlohmann::ordered_json::object();
    nlohmann::ordered_json procedures = nlohmann::ordered_json::object();

    void save();  // atomic: writes catalog.json.tmp, then renames it over catalog.json
    std::string tablePath(const std::string& table) const;
    TableSchema& get(const std::string& table);  // throws ExecutionError if missing
    TableSchema* find(const std::string& table);  // nullptr if missing
    void add(const TableSchema& schema);          // sets + saves
    void remove(const std::string& table);
    void addView(const std::string& name, const std::string& queryText);
    void removeView(const std::string& name);

    const std::string& dbDir() const { return dbDir_; }
    const std::string& path() const { return path_; }

private:
    std::string dbDir_;
    std::string path_;
    void load();
};

}  // namespace meradb
