// cpp/include/meradb/wb_tree.h
//
// The schema tree of the workbench (mirrors tui.py's Tree handling and refresh_schema): pure model, no terminal.
#pragma once
#include "meradb/wb_text.h"
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace meradb::wb {

struct NodeKey {
    enum class Kind { Root, Database, Table, Column };
    Kind kind = Kind::Root;
    std::string db, table, column;
    bool operator==(const NodeKey& o) const { return kind == o.kind && db == o.db && table == o.table && column == o.column; }
    bool operator<(const NodeKey& o) const;  // any strict weak order
};

struct TreeRow {
    int depth = 0;           // Root 0, Database 1, Table 2, Column 3
    NodeKey key;
    Line label;              // styled, WITHOUT the expander marker
    bool expandable = false; // Root, Database, Table (Textual lets an empty database toggle too); false for a Column
    bool expanded = false;   // the stored flag, even for an expandable node without children; always false for a Column
};

// What pressing Enter on a row asks for (tui.py on_tree_node_selected).
struct TreeActivation {
    bool toggled = false;                 // the row's expansion flipped (Textual does this on select)
    std::vector<std::string> scripts;     // run each with run_text, in order
    std::string insertText;               // a column name to insert at the editor cursor
};

class TreeModel {
public:
    TreeModel();                                                 // only the root "Databases", expanded
    // Rebuilds from Engine::schemaTree() JSON. Expansion state is kept per node key; a database is expanded if it
    // is the current one OR was expanded before, a table only if it was expanded before (tui.py refresh_schema).
    void refresh(const nlohmann::ordered_json& databases);
    const std::vector<TreeRow>& rows() const { return rows_; }            // the visible rows, root first
    std::vector<TreeRow> allNodes() const;                                // every node, depth first, expanded or not
    int selected() const { return scroll_.cursor(); }
    void select(int row);
    void moveSelection(int delta);
    void toggle(int row);                                                 // no-op on a column
    bool expandOrDescend();                                               // Right: expand, or move to the first child
    bool collapseOrAscend();                                              // Left: collapse, or move to the parent
    TreeActivation activate(int row, const std::string& currentDb);
    ScrollState& scroll() { return scroll_; }
    const ScrollState& scroll() const { return scroll_; }

private:
    nlohmann::ordered_json data_ = nlohmann::ordered_json::array();
    std::set<NodeKey> expanded_;
    bool rootExpanded_ = true;
    std::vector<TreeRow> rows_;
    ScrollState scroll_;
    void rebuild();
};

}  // namespace meradb::wb
