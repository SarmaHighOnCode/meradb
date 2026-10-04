// cpp/tests/wb_probe.cpp -- runs one scenario of workbench_scenarios.json against the C++ workbench Session and prints
// what the workbench shows as one line of JSON, in the same shape as workbench_pilot.py prints for the Python workbench.
//
//   wb_probe <scenarios.json> <name> --data <folder> --cwd <folder>
#include "wb_test_util.h"
#include "meradb/backend.h"
#include "meradb/wb_dialogs.h"
#include "meradb/wb_form.h"
#include "meradb/wb_session.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

using json = nlohmann::ordered_json;
using namespace meradb;
using namespace meradb::wb;

namespace {

std::string readBinary(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The text of the newest exports/*.csv below `cwd` (the stamps sort like the times), or nullopt.
bool newestCsv(const std::filesystem::path& cwd, std::string& out) {
    std::error_code ec;
    const std::filesystem::path dir = cwd / "exports";
    if (!std::filesystem::is_directory(dir, ec)) return false;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.path().extension() == ".csv") files.push_back(entry.path());
    if (files.empty()) return false;
    std::sort(files.begin(), files.end());
    out = readBinary(files.back());
    return true;
}

json snapshot(Session& session, const std::filesystem::path& cwd) {
    json out = json::object();
    out["subtitle"] = session.subtitle();
    out["editor"] = session.editor().text();
    out["history"] = session.history().items();
    const ResultTable* table = session.table();
    out["results_title"] = resultsTitle(table);
    json columns = json::array();
    json rows = json::array();
    if (table) {
        for (const std::string& column : table->columns) columns.push_back(column);
        for (const auto& row : table->rows) {
            json cells = json::array();
            for (const Cell& cell : row) cells.push_back(json{{"t", cell.text}, {"k", cellKindName(cell.kind)}});
            rows.push_back(cells);
        }
    }
    out["columns"] = columns;
    out["rows"] = rows;
    json log = json::array();
    for (const LogEntry& entry : session.log().entries()) {
        std::string text;
        for (std::size_t i = 0; i < entry.lines.size(); ++i) {
            if (i) text += "\n";
            text += plainText(entry.lines[i]);
        }
        log.push_back(json{{"k", logKindName(entry.kind)}, {"t", text}});
    }
    out["log"] = log;
    json tree = json::array();
    for (const TreeRow& row : session.tree().allNodes())
        tree.push_back(json{{"depth", row.depth}, {"label", plainText(row.label)}, {"expanded", row.expanded}});
    out["tree"] = tree;
    std::string csv;
    if (newestCsv(cwd, csv)) out["csv"] = csv;
    return out;
}

// Enter on the visible tree row whose chain of labels (root first) equals `path`.
void treeEnter(Session& session, const std::vector<std::string>& path) {
    std::vector<std::string> chain;
    const auto& rows = session.tree().rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        chain.resize(static_cast<std::size_t>(rows[i].depth));
        chain.push_back(plainText(rows[i].label));
        if (chain == path) {
            session.activateTreeRow(static_cast<int>(i));
            return;
        }
    }
    throw std::runtime_error("no visible tree row for the path");
}

// The connect dialog as Textual's names for its keys address it: "tab", "shift+tab", "left", "right", "home", "end",
// "backspace", "delete", "space", or one character.
ftxui::Event dialogKey(const std::string& name) {
    if (name == "tab") return ftxui::Event::Tab;
    if (name == "shift+tab") return ftxui::Event::TabReverse;
    if (name == "left") return ftxui::Event::ArrowLeft;
    if (name == "right") return ftxui::Event::ArrowRight;
    if (name == "home") return ftxui::Event::Home;
    if (name == "end") return ftxui::Event::End;
    if (name == "backspace") return ftxui::Event::Backspace;
    if (name == "delete") return ftxui::Event::Delete;
    if (name == "space") return ftxui::Event::Character(" ");
    return ftxui::Event::Character(name);
}

// Same shape as observe_dialog() in workbench_pilot.py. A selected field is [0, length] (Textual: start 0, end length),
// otherwise the cursor twice.
json dialogSnapshot(const ConnectForm& form) {
    static const char* const ids[] = {"host", "port", "password", "database", "server", "local", "cancel"};
    json fields = json::array();
    for (int i = 0; i < ConnectForm::kFields; ++i) {
        const LineEdit& f = form.field(i);
        const int a = f.selected() ? 0 : f.cursor();
        fields.push_back(json{{"text", f.text()}, {"selection", json::array({a, f.cursor()})}});
    }
    return json{{"focus", ids[form.active()]}, {"fields", fields}};
}

}  // namespace

int main(int argc, char** argv) {
    std::string scenariosPath, name, dataDir, cwd;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--data" && i + 1 < argc) dataDir = argv[++i];
        else if (arg == "--cwd" && i + 1 < argc) cwd = argv[++i];
        else positional.push_back(arg);
    }
    if (positional.size() != 2 || dataDir.empty() || cwd.empty()) {
        std::cerr << "usage: wb_probe <scenarios.json> <name> --data <folder> --cwd <folder>\n";
        return 2;
    }
    scenariosPath = positional[0];
    name = positional[1];
    try {
        const json scenarios = json::parse(readBinary(std::filesystem::u8path(scenariosPath)));
        const json* scenario = nullptr;
        for (const json& s : scenarios)
            if (s.at("name") == name) scenario = &s;
        if (!scenario) throw std::runtime_error("no such scenario: " + name);

        wbtest::ManualPoster poster;
        SessionOptions options;
        options.dataDir = dataDir;
        options.exportBaseDir = cwd;
        json snaps = json::object();
        {
            Session session(std::make_unique<LocalBackend>(dataDir), options, poster.poster());
            auto settle = [&] {
                if (!poster.pumpIdle(session)) throw std::runtime_error("the session did not become idle");
            };
            settle();
            std::unique_ptr<ConnectForm> form;   // the connect dialog, between dialog_open and dialog_close
            for (const json& step : scenario->at("steps")) {
                if (step.contains("dialog_open")) {
                    const ConnectDefaults defaults = session.connectDefaults();
                    form.reset(new ConnectForm(defaults.host, defaults.port));
                } else if (step.contains("dialog_keys")) {
                    for (const std::string& key : step["dialog_keys"].get<std::vector<std::string>>())
                        handleConnectEvent(*form, dialogKey(key), session);
                } else if (step.contains("dialog_snapshot")) {
                    snaps[step["dialog_snapshot"].get<std::string>()] = dialogSnapshot(*form);
                } else if (step.contains("dialog_close")) {
                    form.reset();
                } else if (step.contains("set_text")) {
                    session.editor().setText(step["set_text"].get<std::string>());
                } else if (step.contains("select")) {
                    const auto& s = step["select"];
                    session.editor().selectRange(Pos{s[0].get<int>(), s[1].get<int>()}, Pos{s[2].get<int>(), s[3].get<int>()});
                } else if (step.contains("run")) {
                    session.runEditorText();
                } else if (step.contains("explain")) {
                    session.explainEditorText();
                } else if (step.contains("history")) {
                    session.historyStep(step["history"].get<int>());
                } else if (step.contains("export")) {
                    session.exportCsv();
                } else if (step.contains("tree_enter")) {
                    treeEnter(session, step["tree_enter"].get<std::vector<std::string>>());
                } else if (step.contains("clear_log")) {
                    session.clearLog();
                } else if (step.contains("connect")) {
                    session.connect(makeConnectRequest(true, "127.0.0.1", "6372", "", ""));
                } else if (step.contains("snapshot")) {
                    snaps[step["snapshot"].get<std::string>()] = snapshot(session, std::filesystem::u8path(cwd));
                }
                settle();
            }
            session.requestQuit();
            poster.pumpUntil([&] { return !session.busy() && session.quitting(); });
            session.shutdown();
        }
        const std::string text = snaps.dump(-1, ' ', false, json::error_handler_t::replace);
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fputc('\n', stdout);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "wb_probe: " << e.what() << "\n";
        return 1;
    }
}
