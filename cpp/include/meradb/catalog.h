// cpp/include/meradb/catalog.h
//
// The CATALOG: the database's "data about data". It remembers which tables
// (and views) exist and what their columns are; the rows themselves live in
// <table>.tbl heap files (see storage.h). Mirrors meradb/catalog.py and
// reads/writes the same catalog.json shape, so a database directory can be
// opened by either engine.
#pragma once
#include "meradb/datatypes.h"
#include "meradb/ordered_map.h"
#include <nlohmann/json.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace meradb {

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
