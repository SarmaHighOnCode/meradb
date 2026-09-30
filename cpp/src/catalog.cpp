// cpp/src/catalog.cpp
#include "meradb/catalog.h"
#include "meradb/errors.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace meradb {

namespace {

// ---- non-finite floats ----
// Python's json writes float('inf') / -inf / nan as the bare tokens
// Infinity / -Infinity / NaN (not valid JSON, but json.load accepts them).
// nlohmann neither reads nor writes those, so:
//  * reading: quoteNonFinite() turns bare tokens (outside string literals)
//    into marker strings before parsing, and ONLY a FLOAT column's default
//    turns a marker back into a double -- a TEXT/DATE default that happens
//    to equal the marker is kept as the string it is;
//  * writing: Catalog::save() puts markers only at the exact spots that hold
//    a non-finite default, using a nonce that occurs nowhere else in the
//    document, then swaps those quoted markers for the bare tokens.
const std::string kNonFiniteMarker = std::string("\0meradb-nonfinite:", 18);
const char* const kNonFiniteTokens[] = {"-Infinity", "Infinity", "NaN"};

const char* nonFiniteToken(double d) {
    if (std::isnan(d)) return "NaN";
    return d < 0 ? "-Infinity" : "Infinity";
}

std::optional<double> fromMarker(const std::string& s) {
    if (s.compare(0, kNonFiniteMarker.size(), kNonFiniteMarker) != 0) return std::nullopt;
    std::string token = s.substr(kNonFiniteMarker.size());
    if (token == "Infinity") return std::numeric_limits<double>::infinity();
    if (token == "-Infinity") return -std::numeric_limits<double>::infinity();
    if (token == "NaN") return std::numeric_limits<double>::quiet_NaN();
    return std::nullopt;
}

// Bare Infinity/-Infinity/NaN tokens OUTSIDE string literals -> marker strings.
std::string quoteNonFinite(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool inString = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < text.size()) out += text[++i];
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out += c;
            continue;
        }
        bool replaced = false;
        for (const char* token : kNonFiniteTokens) {
            size_t len = std::strlen(token);
            if (text.compare(i, len, token) == 0) {
                out += "\"\\u0000meradb-nonfinite:";
                out += token;
                out += '"';
                i += len - 1;
                replaced = true;
                break;
            }
        }
        if (!replaced) out += c;
    }
    return out;
}

// dump() the way Python's json.dump(indent=2) writes (ensure_ascii). A
// library error (e.g. a string that isn't valid UTF-8) becomes a StorageError.
std::string renderJson(const json& j) {
    try {
        return j.dump(2, ' ', true);
    } catch (const nlohmann::json::exception& e) {
        throw StorageError(std::string("catalog.json likh nahi paaye: ") + e.what());
    }
}

// Non-finite doubles come out as null here (as nlohmann would write them);
// Catalog::save() patches those spots with Python's bare tokens.
json valueToJson(const std::optional<Value>& v) {
    if (!v.has_value() || v->isNull()) return nullptr;
    const auto& d = v->data;
    if (std::holds_alternative<int64_t>(d)) return std::get<int64_t>(d);
    if (std::holds_alternative<double>(d)) {
        double x = std::get<double>(d);
        return std::isfinite(x) ? json(x) : json(nullptr);
    }
    if (std::holds_alternative<bool>(d)) return std::get<bool>(d);
    if (std::holds_alternative<std::string>(d)) return std::get<std::string>(d);
    if (std::holds_alternative<Date>(d)) return std::get<Date>(d).isoFormat();  // like Column.to_dict
    return nullptr;
}

// The raw JSON literal of a WARNA default, before it's coerced to the column type.
Value jsonToValue(const json& j, const std::string& column, const std::string& typeName) {
    if (j.is_null()) return Value();
    if (j.is_boolean()) return Value(j.get<bool>());
    if (j.is_number_integer()) {
        if (j.is_number_unsigned() && j.get<uint64_t>() > static_cast<uint64_t>(INT64_MAX))
            return Value(j.get<double>());  // out of INT range: let coerce() report it
        return Value(j.get<int64_t>());
    }
    if (j.is_number_float()) return Value(j.get<double>());
    if (j.is_string()) {
        const auto& s = j.get_ref<const std::string&>();
        if (typeName == "FLOAT")  // only a FLOAT default can be a bare Infinity/NaN token
            if (auto d = fromMarker(s)) return Value(*d);
        return Value(s);
    }
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
    if (def != j.end() && !def->is_null()) c.defaultValue = coerce(jsonToValue(*def, c.name, c.typeName), c.typeName, c.name);
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
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    json data;
    try {
        data = json::parse(quoteNonFinite(text));
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
    std::string text = renderJson(data);

    // Non-finite FLOAT defaults: Python writes bare Infinity/-Infinity/NaN.
    // Mark exactly those spots with a marker whose escaped form appears
    // nowhere in the document, then replace the quoted markers by the tokens.
    std::vector<std::pair<json*, const char*>> nonFinite;
    for (const auto& [name, schema] : tables) {
        for (size_t i = 0; i < schema.columns.size(); ++i) {
            const auto& def = schema.columns[i].defaultValue;
            if (def && std::holds_alternative<double>(def->data) && !std::isfinite(std::get<double>(def->data)))
                nonFinite.emplace_back(&data["tables"][name]["columns"][i]["default"],
                                       nonFiniteToken(std::get<double>(def->data)));
        }
    }
    if (!nonFinite.empty()) {
        std::string nonce;
        for (unsigned k = 0;; ++k) {
            nonce = "meradb-nonfinite-" + std::to_string(k) + ":";
            if (text.find("\\u0000" + nonce) == std::string::npos) break;
        }
        for (auto& [slot, token] : nonFinite) *slot = std::string(1, '\0') + nonce + token;
        text = renderJson(data);
        for (const char* token : kNonFiniteTokens) {
            std::string quoted = "\"\\u0000" + nonce + token + "\"";
            for (size_t pos = text.find(quoted); pos != std::string::npos; pos = text.find(quoted, pos))
                text.replace(pos, quoted.size(), token);
        }
    }
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) throw StorageError(tmp + " likh nahi paaye");
        out << text;
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

// add/addView: check the new entry serialises BEFORE touching memory, and
// undo the change if writing the file fails, so memory and catalog.json
// never disagree. (remove/removeView can only fail on I/O; like Python,
// the entry is then already gone from memory.)
void Catalog::add(const TableSchema& schema) {
    renderJson(schema.toJson());
    std::optional<TableSchema> previous;
    if (auto* existing = find(schema.name)) previous = *existing;
    tables[schema.name] = schema;
    try {
        save();
    } catch (...) {
        if (previous) tables[schema.name] = std::move(*previous);
        else tables.erase(schema.name);
        throw;
    }
}

void Catalog::remove(const std::string& table) {
    get(table);  // missing table: same error as get()
    tables.erase(table);
    save();
}

void Catalog::addView(const std::string& name, const std::string& queryText) {
    renderJson(json(queryText));
    std::optional<std::string> previous;
    if (auto it = views.find(name); it != views.end()) previous = it->second;
    views[name] = queryText;
    try {
        save();
    } catch (...) {
        if (previous) views[name] = std::move(*previous);
        else views.erase(name);
        throw;
    }
}

void Catalog::removeView(const std::string& name) {
    if (views.erase(name) == 0) throw ExecutionError("View '" + name + "' exist nahi karta");
    save();
}

void Catalog::addTrigger(const std::string& name, const std::string& timing, const std::string& event,
                         const std::string& table, const std::string& bodyText) {
    json entry = json::object();  // key order = Python's dict literal order
    entry["timing"] = timing;
    entry["event"] = event;
    entry["table"] = table;
    entry["body_text"] = bodyText;
    renderJson(entry);  // fails early (invalid UTF-8) before memory is touched
    std::optional<json> previous;
    if (triggers.contains(name)) previous = triggers[name];
    triggers[name] = entry;
    try {
        save();
    } catch (...) {
        if (previous) triggers[name] = *previous;
        else triggers.erase(name);
        throw;
    }
}

void Catalog::removeTrigger(const std::string& name) {
    triggers.erase(name);
    save();
}

std::vector<Catalog::TriggerInfo> Catalog::triggersFor(const std::string& timing, const std::string& event,
                                                       const std::string& table) const {
    std::vector<TriggerInfo> out;
    for (auto it = triggers.begin(); it != triggers.end(); ++it) {
        const json& t = it.value();
        if (t.value("timing", "") == timing && t.value("event", "") == event && t.value("table", "") == table)
            out.push_back({it.key(), timing, event, table, t.value("body_text", "")});
    }
    return out;
}

void Catalog::addProcedure(const std::string& name, const std::vector<std::pair<std::string, std::string>>& params,
                           const std::string& bodyText) {
    json entry = json::object();
    json list = json::array();
    for (const auto& [pname, ptype] : params) list.push_back(json::array({pname, ptype}));
    entry["params"] = std::move(list);
    entry["body_text"] = bodyText;
    renderJson(entry);
    std::optional<json> previous;
    if (procedures.contains(name)) previous = procedures[name];
    procedures[name] = entry;
    try {
        save();
    } catch (...) {
        if (previous) procedures[name] = *previous;
        else procedures.erase(name);
        throw;
    }
}

void Catalog::removeProcedure(const std::string& name) {
    procedures.erase(name);
    save();
}

std::optional<Catalog::ProcedureInfo> Catalog::findProcedure(const std::string& name) const {
    auto it = procedures.find(name);
    if (it == procedures.end()) return std::nullopt;
    ProcedureInfo info;
    try {
        for (const auto& p : it->at("params")) info.params.emplace_back(p.at(0).get<std::string>(), p.at(1).get<std::string>());
        info.bodyText = it->at("body_text").get<std::string>();
    } catch (const nlohmann::json::exception&) {
        throw StorageError("catalog.json corrupt hai: procedure '" + name + "' samajh nahi aaya");
    }
    return info;
}

}  // namespace meradb
