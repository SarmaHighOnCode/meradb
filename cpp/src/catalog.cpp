// cpp/src/catalog.cpp
#include "meradb/catalog.h"
#include "meradb/errors.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace meradb {

namespace {

json valueToJson(const std::optional<Value>& v) {
    if (!v.has_value() || v->isNull()) return nullptr;
    const auto& d = v->data;
    if (std::holds_alternative<int64_t>(d)) return std::get<int64_t>(d);
    if (std::holds_alternative<double>(d)) return std::get<double>(d);
    if (std::holds_alternative<bool>(d)) return std::get<bool>(d);
    if (std::holds_alternative<std::string>(d)) return std::get<std::string>(d);
    if (std::holds_alternative<Date>(d)) return std::get<Date>(d).isoFormat();  // like Column.to_dict
    return nullptr;
}

// The raw JSON literal of a WARNA default, before it's coerced to the column type.
Value jsonToValue(const json& j, const std::string& column) {
    if (j.is_null()) return Value();
    if (j.is_boolean()) return Value(j.get<bool>());
    if (j.is_number_integer()) {
        if (j.is_number_unsigned() && j.get<uint64_t>() > static_cast<uint64_t>(INT64_MAX))
            return Value(j.get<double>());  // out of INT range: let coerce() report it
        return Value(j.get<int64_t>());
    }
    if (j.is_number_float()) return Value(j.get<double>());
    if (j.is_string()) return Value(j.get<std::string>());
    throw StorageError("catalog.json corrupt hai: column '" + column + "' ka WARNA value samajh nahi aaya");
}

template <typename T>
std::optional<T> optionalField(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return it->template get<T>();
}

}  // namespace

// ---------------------------------------------------------------- Column

json Column::toJson() const {
    // Same keys, same order as Python's dataclasses.asdict(Column).
    json j;
    j["name"] = name;
    j["type_name"] = typeName;
    j["primary_key"] = primaryKey;
    j["not_null"] = notNull;
    j["unique"] = unique;
    j["default"] = valueToJson(defaultValue);
    j["max_length"] = maxLength.has_value() ? json(*maxLength) : json(nullptr);
    j["ref_table"] = refTable.has_value() ? json(*refTable) : json(nullptr);
    j["ref_column"] = refColumn.has_value() ? json(*refColumn) : json(nullptr);
    j["check"] = check.has_value() ? json(*check) : json(nullptr);
    return j;
}

Column Column::fromJson(const json& j) {
    Column c;
    c.name = j.at("name").get<std::string>();
    c.typeName = j.at("type_name").get<std::string>();
    // Everything else is optional: older catalog.json files predate these keys.
    c.primaryKey = j.value("primary_key", false);
    c.notNull = j.value("not_null", false);
    c.unique = j.value("unique", false);
    auto def = j.find("default");
    if (def != j.end() && !def->is_null()) c.defaultValue = coerce(jsonToValue(*def, c.name), c.typeName, c.name);
    c.maxLength = optionalField<int>(j, "max_length");
    c.refTable = optionalField<std::string>(j, "ref_table");
    c.refColumn = optionalField<std::string>(j, "ref_column");
    c.check = optionalField<std::string>(j, "check");
    return c;
}

// ---------------------------------------------------------------- TableSchema

std::vector<std::string> TableSchema::columnNames() const {
    std::vector<std::string> names;
    names.reserve(columns.size());
    for (const auto& c : columns) names.push_back(c.name);
    return names;
}

std::vector<std::string> TableSchema::types() const {
    std::vector<std::string> t;
    t.reserve(columns.size());
    for (const auto& c : columns) t.push_back(c.typeName);
    return t;
}

size_t TableSchema::indexOf(const std::string& column) const {
    for (size_t i = 0; i < columns.size(); ++i)
        if (columns[i].name == column) return i;
    throw ExecutionError("Table '" + name + "' mein column '" + column + "' nahi hai");
}

const Column& TableSchema::getColumn(const std::string& column) const { return columns[indexOf(column)]; }

json TableSchema::toJson() const {
    json j;
    j["name"] = name;
    j["columns"] = json::array();
    for (const auto& c : columns) j["columns"].push_back(c.toJson());
    j["composite_unique"] = compositeUnique;
    j["composite_pk"] = compositePk.has_value() ? json(*compositePk) : json(nullptr);
    return j;
}

TableSchema TableSchema::fromJson(const json& j) {
    TableSchema s;
    s.name = j.at("name").get<std::string>();
    for (const auto& cj : j.at("columns")) s.columns.push_back(Column::fromJson(cj));
    // composite_unique / composite_pk are absent in older catalog.json files.
    auto cu = j.find("composite_unique");
    if (cu != j.end() && !cu->is_null()) s.compositeUnique = cu->get<std::vector<std::vector<std::string>>>();
    s.compositePk = optionalField<std::vector<std::string>>(j, "composite_pk");
    return s;
}

// ---------------------------------------------------------------- Catalog

Catalog::Catalog(std::string dbDir) : dbDir_(std::move(dbDir)) {
    path_ = (fs::path(dbDir_) / kFileName).string();
    load();
}

void Catalog::load() {
    tables.clear();
    views.clear();
    triggers = json::object();
    procedures = json::object();
    if (!fs::exists(path_)) return;  // fresh database: empty catalog

    std::ifstream in(path_);
    if (!in) throw StorageError(path_ + " khul nahi paayi");
    json data;
    try {
        data = json::parse(in);
        for (const auto& [name, tj] : data.at("tables").items()) tables[name] = TableSchema::fromJson(tj);
        // "views"/"triggers"/"procedures" keys are absent in older catalog.json files
        if (auto it = data.find("views"); it != data.end())
            for (const auto& [name, vj] : it->items()) views[name] = vj.get<std::string>();
        if (auto it = data.find("triggers"); it != data.end()) triggers = *it;
        if (auto it = data.find("procedures"); it != data.end()) procedures = *it;
    } catch (const nlohmann::json::exception& e) {
        throw StorageError(path_ + " corrupt hai: " + e.what());
    }
}

void Catalog::save() {
    json tablesJson = json::object();
    for (const auto& [name, schema] : tables) tablesJson[name] = schema.toJson();
    json viewsJson = json::object();
    for (const auto& [name, text] : views) viewsJson[name] = text;

    json data;
    data["tables"] = std::move(tablesJson);
    data["views"] = std::move(viewsJson);
    data["triggers"] = triggers;
    data["procedures"] = procedures;

    // Never leave a half-written catalog behind: write a temp file, then
    // swap it in (Python: os.replace). Text mode on purpose, so line endings
    // match what Python's open(..., "w") writes on the same platform; and
    // ensure_ascii matches json.dump's default escaping.
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) throw StorageError(tmp + " likh nahi paaye");
        out << data.dump(2, ' ', true);
        out.flush();
        if (!out) throw StorageError(tmp + " likh nahi paaye");
    }
    std::error_code ec;
    fs::rename(tmp, path_, ec);
    if (ec) throw StorageError(path_ + " save nahi hua: " + ec.message());
}

std::string Catalog::tablePath(const std::string& table) const {
    return (fs::path(dbDir_) / (table + ".tbl")).string();
}

TableSchema& Catalog::get(const std::string& table) {
    TableSchema* schema = find(table);
    if (schema == nullptr) throw ExecutionError("Table '" + table + "' exist nahi karta");
    return *schema;
}

TableSchema* Catalog::find(const std::string& table) {
    auto it = tables.find(table);
    return it == tables.end() ? nullptr : &it->second;
}

void Catalog::add(const TableSchema& schema) {
    tables[schema.name] = schema;
    save();
}

void Catalog::remove(const std::string& table) {
    get(table);  // missing table: same error as get()
    tables.erase(table);
    save();
}

void Catalog::addView(const std::string& name, const std::string& queryText) {
    views[name] = queryText;
    save();
}

void Catalog::removeView(const std::string& name) {
    if (views.erase(name) == 0) throw ExecutionError("View '" + name + "' exist nahi karta");
    save();
}

}  // namespace meradb
