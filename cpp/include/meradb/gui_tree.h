// cpp/include/meradb/gui_tree.h -- the schema panel of the desktop window: filter box and tree (databases, tables,
// columns) over wb::TreeModel, drawn with Dear ImGui.
#pragma once
#include "meradb/gui_fonts.h"
#include "meradb/gui_theme.h"
#include "meradb/wb_session.h"
#include "imgui.h"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace meradb::gui {

// One line of the tree as it is shown.
struct TreeEntry {
    wb::NodeKey key;
    int depth = 1;                   // 1 database, 2 table, 3 column
    std::string name;
    std::string type;                // columns: the type name, lower case
    std::vector<std::string> tags;   // columns: PK / UQ / NN
    bool current = false;            // the database in use
    bool expandable = false;
    bool expanded = false;
};

// Case-insensitive (ASCII) substring test; an empty filter matches everything.
bool matchesFilter(const std::string& name, const std::string& filter);

// `nodes` is TreeModel::allNodes() (depth first, root first) or rows(). The root is never listed.
//  - empty filter: everything below an expanded node, as the model has it;
//  - otherwise: the nodes whose own name matches plus their parents; all of those are shown open. The children of a
//    matching node that do not match themselves stay hidden.
std::vector<TreeEntry> treeEntries(const std::vector<wb::TreeRow>& nodes, const std::string& filter);

// The statements the "Top 100 rows dikhao" menu item runs for a table: ISTEMAL first when it lives in another database.
std::vector<std::string> topRowsScripts(const wb::NodeKey& key, const std::string& currentDb);
// What "Naam editor mein daalo" / "Naam copy karo" use: the database, table or column name.
std::string nodeName(const wb::NodeKey& key);

class SchemaTree {
public:
    // `editing` runs a change of the editor text (activating a column inserts its name) as one undo step and puts the
    // caret in the editor.
    SchemaTree(wb::Session& session, std::function<void(const std::function<void()>&)> editing)
        : session_(session), editing_(std::move(editing)) {}

    // The filter box and the list; fills the rest of the current window.
    void draw(const Theme& theme, const FontSet& fonts, float scale);

    std::string filter() const { return filter_; }
    void setFilter(const std::string& text);
    bool activate(const wb::NodeKey& key);            // double-click / Enter
    void select(const wb::NodeKey& key);              // click
    const wb::NodeKey& selected() const { return selected_; }
    bool focused() const { return focused_; }
    void setFocused(bool focused) { focused_ = focused; }

    struct RowHit {
        wb::NodeKey key;
        ImVec2 min, max;
    };
    const std::vector<RowHit>& rowHits() const { return hits_; }   // the rows drawn in the last frame, screen pixels
    const std::vector<TreeEntry>& entries() const { return entries_; }

private:
    int findRow(const wb::NodeKey& key) const;
    int ensureVisible(const wb::NodeKey& key);
    void handleKeys();
    void drawMenu();

    wb::Session& session_;
    std::function<void(const std::function<void()>&)> editing_;
    char filter_[128] = {0};
    wb::NodeKey selected_;
    wb::NodeKey menuKey_;
    bool focused_ = false;
    bool openMenu_ = false;
    bool scrollToSelected_ = false;
    std::vector<TreeEntry> entries_;
    std::vector<RowHit> hits_;
};

}  // namespace meradb::gui
