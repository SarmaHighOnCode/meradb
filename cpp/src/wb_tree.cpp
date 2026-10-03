// cpp/src/wb_tree.cpp
#include "meradb/wb_tree.h"
#include "meradb/pytext.h"
#include <tuple>

namespace meradb::wb {

namespace {

using Kind = NodeKey::Kind;
using Json = nlohmann::ordered_json;

NodeKey rootKey() { return NodeKey{}; }

NodeKey dbKey(const std::string& db) {
    NodeKey k;
    k.kind = Kind::Database;
    k.db = db;
    return k;
}

NodeKey tableKey(const std::string& db, const std::string& table) {
    NodeKey k;
    k.kind = Kind::Table;
    k.db = db;
    k.table = table;
    return k;
}

NodeKey columnKey(const std::string& db, const std::string& table, const std::string& column) {
    NodeKey k;
    k.kind = Kind::Column;
    k.db = db;
    k.table = table;
    k.column = column;
    return k;
}

std::string nameOf(const Json& node) { return node.value("name", std::string()); }

TreeRow makeRow(int depth, const NodeKey& key, Line label, bool expandable, bool expanded) {
    TreeRow row;
    row.depth = depth;
    row.key = key;
    row.label = std::move(label);
    row.expandable = expandable;
    row.expanded = expanded;
    return row;
}

Line dbLabel(const std::string& name, bool current) {
    Line l;
    appendSegment(l, name, current ? fgStyle(palette::kGreen, true) : Style{});
    return l;
}

Line tableLabel(const std::string& name) {
    Line l;
    appendSegment(l, name, fgStyle(-1, true));
    return l;
}

Line columnLabel(const Json& col) {
    Line l;
    appendSegment(l, nameOf(col), Style{});
    appendSegment(l, " ", Style{});
    appendSegment(l, pytext::lower(col.value("type_name", std::string())), fgStyle(palette::kCyan));
    if (col.value("primary_key", false)) appendSegment(l, " PK", fgStyle(palette::kOrange));
    else if (col.value("unique", false)) appendSegment(l, " UQ", fgStyle(palette::kOrange));
    if (col.value("not_null", false)) appendSegment(l, " NN", fgStyle(-1, false, true));
    return l;
}

}  // namespace

bool NodeKey::operator<(const NodeKey& o) const {
    return std::make_tuple(static_cast<int>(kind), db, table, column) <
           std::make_tuple(static_cast<int>(o.kind), o.db, o.table, o.column);
}

TreeModel::TreeModel() { rebuild(); }

void TreeModel::rebuild() {
    rows_.clear();
    Line root;
    appendSegment(root, "Databases", Style{});
    rows_.push_back(makeRow(0, rootKey(), std::move(root), true, rootExpanded_));
    if (rootExpanded_ && data_.is_array()) {
        for (const Json& db : data_) {
            const std::string dbName = nameOf(db);
            const NodeKey dk = dbKey(dbName);
            const bool dbOpen = expanded_.count(dk) != 0;
            rows_.push_back(makeRow(1, dk, dbLabel(dbName, db.value("current", false)), true, dbOpen));
            if (!dbOpen || !db.contains("tables")) continue;
            for (const Json& table : db["tables"]) {
                const std::string tName = nameOf(table);
                const NodeKey tk = tableKey(dbName, tName);
                const bool tOpen = expanded_.count(tk) != 0;
                rows_.push_back(makeRow(2, tk, tableLabel(tName), true, tOpen));
                if (!tOpen || !table.contains("columns")) continue;
                for (const Json& col : table["columns"])
                    rows_.push_back(makeRow(3, columnKey(dbName, tName, nameOf(col)), columnLabel(col), false, false));
            }
        }
    }
    scroll_.setCount(static_cast<int>(rows_.size()));
}

std::vector<TreeRow> TreeModel::allNodes() const {
    std::vector<TreeRow> out;
    Line root;
    appendSegment(root, "Databases", Style{});
    out.push_back(makeRow(0, rootKey(), std::move(root), true, rootExpanded_));
    if (!data_.is_array()) return out;
    for (const Json& db : data_) {
        const std::string dbName = nameOf(db);
        const NodeKey dk = dbKey(dbName);
        out.push_back(makeRow(1, dk, dbLabel(dbName, db.value("current", false)), true, expanded_.count(dk) != 0));
        if (!db.contains("tables")) continue;
        for (const Json& table : db["tables"]) {
            const std::string tName = nameOf(table);
            const NodeKey tk = tableKey(dbName, tName);
            out.push_back(makeRow(2, tk, tableLabel(tName), true, expanded_.count(tk) != 0));
            if (!table.contains("columns")) continue;
            for (const Json& col : table["columns"])
                out.push_back(makeRow(3, columnKey(dbName, tName, nameOf(col)), columnLabel(col), false, false));
        }
    }
    return out;
}

void TreeModel::refresh(const nlohmann::ordered_json& databases) {
    const int oldIndex = scroll_.cursor();
    NodeKey selectedKey = rootKey();
    bool hadSelection = false;
    if (oldIndex >= 0 && oldIndex < static_cast<int>(rows_.size())) {
        selectedKey = rows_[static_cast<std::size_t>(oldIndex)].key;
        hadSelection = true;
    }
    std::set<NodeKey> next;
    if (databases.is_array()) {
        for (const Json& db : databases) {
            const std::string dbName = nameOf(db);
            const NodeKey dk = dbKey(dbName);
            if (db.value("current", false) || expanded_.count(dk)) next.insert(dk);
            if (!db.contains("tables")) continue;
            for (const Json& table : db["tables"]) {
                const NodeKey tk = tableKey(dbName, nameOf(table));
                if (expanded_.count(tk)) next.insert(tk);
            }
        }
    }
    expanded_ = std::move(next);
    data_ = databases;
    rebuild();
    if (hadSelection) {
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].key == selectedKey) {
                scroll_.setCursor(static_cast<int>(i));
                return;
            }
        }
    }
    scroll_.setCursor(oldIndex);
}

void TreeModel::select(int row) { scroll_.setCursor(row); }

void TreeModel::moveSelection(int delta) { scroll_.moveCursor(delta); }

void TreeModel::toggle(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const TreeRow& r = rows_[static_cast<std::size_t>(row)];
    if (!r.expandable) return;
    if (r.key.kind == Kind::Root) {
        rootExpanded_ = !rootExpanded_;
    } else if (expanded_.count(r.key)) {
        expanded_.erase(r.key);
    } else {
        expanded_.insert(r.key);
    }
    rebuild();
}

bool TreeModel::expandOrDescend() {
    const int row = scroll_.cursor();
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    const TreeRow r = rows_[static_cast<std::size_t>(row)];
    if (!r.expandable) return false;
    if (!r.expanded) {
        toggle(row);
        return true;
    }
    if (row + 1 < static_cast<int>(rows_.size()) && rows_[static_cast<std::size_t>(row + 1)].depth > r.depth) {
        scroll_.setCursor(row + 1);
        return true;
    }
    return false;
}

bool TreeModel::collapseOrAscend() {
    const int row = scroll_.cursor();
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    const TreeRow r = rows_[static_cast<std::size_t>(row)];
    if (r.expandable && r.expanded) {
        toggle(row);
        return true;
    }
    for (int i = row - 1; i >= 0; --i) {
        if (rows_[static_cast<std::size_t>(i)].depth < r.depth) {
            scroll_.setCursor(i);
            return true;
        }
    }
    return false;
}

TreeActivation TreeModel::activate(int row, const std::string& currentDb) {
    TreeActivation act;
    if (row < 0 || row >= static_cast<int>(rows_.size())) return act;
    const NodeKey key = rows_[static_cast<std::size_t>(row)].key;
    const bool expandable = rows_[static_cast<std::size_t>(row)].expandable;
    if (expandable) {
        toggle(row);
        act.toggled = true;
    }
    switch (key.kind) {
        case Kind::Root: break;
        case Kind::Column: act.insertText = key.column; break;
        case Kind::Database:
            if (key.db != currentDb) act.scripts.push_back("ISTEMAL " + key.db + ";");
            break;
        case Kind::Table:
            if (key.db != currentDb) act.scripts.push_back("ISTEMAL " + key.db + ";");
            act.scripts.push_back("DIKHAO * SE " + key.table + " SIRF 100;");
            break;
    }
    return act;
}

}  // namespace meradb::wb
