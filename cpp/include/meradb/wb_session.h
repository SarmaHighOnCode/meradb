// cpp/include/meradb/wb_session.h
//
// The workbench controller (mirrors the MeraDBApp class of meradb/tui.py without any widgets). It owns the
// Backend and one Worker. Threading contract:
//   * every public method is called on the UI thread only;
//   * the Backend is touched ONLY by jobs running on the worker;
//   * a job never changes session state: it produces a plain-data outcome and hands a closure to the UiPoster,
//     which runs it on the UI thread (ScreenInteractive::Post in the program, a manual queue in tests).
#pragma once
#include "meradb/backend.h"
#include "meradb/wb_editor.h"
#include "meradb/wb_text.h"
#include "meradb/wb_tree.h"
#include "meradb/wb_worker.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace meradb::wb {

using UiPoster = std::function<void(std::function<void()>)>;

enum class Panel { Tree, Results, Log, Editor };
enum class Modal { None, Help, Connect };

// tui.py ConnectScreen._choice(): values stripped; host / port default when empty; password / database absent when empty.
struct ConnectRequest {
    bool local = false;
    std::string host;   // never empty
    std::string port;   // never empty; digits (validated when connecting, like Python's int())
    std::optional<std::string> password;
    std::optional<std::string> database;
};
ConnectRequest makeConnectRequest(bool local, const std::string& host, const std::string& port,
                                  const std::string& password, const std::string& database);

// What the dialog starts with: the current server's host and port from Backend::description() ("host:port"); for
// "local (...)" or anything that does not parse: 127.0.0.1 and 6372 (Python: getattr(backend, "host", DEFAULT_HOST)).
struct ConnectDefaults { std::string host; std::string port; };
ConnectDefaults connectDefaults(const std::string& description);

using BackendFactory = std::function<std::unique_ptr<Backend>(const ConnectRequest&, const std::string& dataDir)>;
// LocalBackend(dataDir) for local; else Connection(host, int(port), password, database). A port that is not an
// integer throws std::invalid_argument("invalid literal for int() with base 10: '<text>'") (Python's ValueError).
std::unique_ptr<Backend> defaultBackendFactory(const ConnectRequest& request, const std::string& dataDir);

struct SessionOptions {
    std::string dataDir;                     // for "Local mode" (the --data option)
    std::string exportBaseDir;               // exports/ goes below this; "" = the current directory
    BackendFactory factory;                  // empty = defaultBackendFactory
    std::function<std::string()> stamp;      // empty = exportFileStamp
};

class Session {
public:
    // Takes the backend (created on the UI thread by the CLI), reads its description / db / transaction state
    // (no I/O), logs the two start-up lines, starts the worker and queues the first schema load.
    Session(std::unique_ptr<Backend> backend, SessionOptions options, UiPoster poster);
    ~Session();  // shutdown()
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void setOnExit(std::function<void()> onExit);   // called on the UI thread once the backend is closed after requestQuit()

    // ---- state the view reads ----
    std::string title() const { return "MeraDB Workbench"; }
    std::string subtitle() const;                    // "<description>  |  db: <db>[  |  TRANSACTION (PAKKA / WAPAS)]"
    const std::string& currentDb() const { return header_.db; }
    bool inTransaction() const { return header_.inTransaction; }
    std::string busyLabel() const;                   // "", "[chal raha hai]", "[chal raha hai +N]", "[band ho raha hai ...]"
    bool busy() const { return pending_ > 0; }
    int pendingJobs() const { return pending_; }
    bool quitting() const { return quitting_; }
    TextBuffer& editor() { return editor_; }
    const LogBuffer& log() const { return log_; }
    TreeModel& tree() { return tree_; }
    const ResultTable* table() const { return table_ ? &*table_ : nullptr; }   // null before the first result
    const Result* lastResult() const { return lastResult_ ? &*lastResult_ : nullptr; }
    int resultVersion() const { return resultVersion_; }                       // bumps on every new table
    const History& history() const { return history_; }
    Modal modal() const { return modal_; }
    std::optional<Panel> takeFocusRequest();         // set by history / column insert (Python: editor.focus())
    ConnectDefaults connectDefaults() const;         // for the dialog

    // ---- actions (key bindings call these) ----
    void runEditorText();                            // F5 / Ctrl+R
    void runText(const std::string& text);           // tui.py run_text
    void clearLog();                                 // Ctrl+L
    void explainEditorText();                        // F6
    void historyStep(int delta);                     // Ctrl+Up (-1) / Ctrl+Down (+1)
    void exportCsv();                                // Ctrl+S
    void openConnectDialog();                        // Ctrl+O
    void connect(const ConnectRequest& request);     // dialog buttons
    void showHelp();                                 // F1
    void closeModal();
    void activateTreeRow(int row);                   // Enter on the tree
    void logLine(LogKind kind, const std::string& text);
    void requestQuit();                              // Ctrl+Q
    void shutdown();                                 // blocks until the worker has closed the backend; idempotent

private:
    struct Header { std::string description; std::string db; bool inTransaction = false; };
    struct Snapshot;    // worker -> UI plain data
    struct RunOutcome;
    struct ConnectOutcome;

    // worker thread only
    Snapshot takeSnapshot();
    void runOnWorker(const std::string& text, RunOutcome& out);
    void closeBackendOnWorker();
    // UI thread only
    void enqueueRun(const std::string& text);
    void applyRun(RunOutcome out);
    void applyConnect(const ConnectOutcome& out);
    void applySnapshot(const Snapshot& snapshot);
    void showResult(const Result& result);

    Header header_;
    std::unique_ptr<Backend> backend_;   // worker thread only
    SessionOptions options_;
    UiPoster post_;
    TextBuffer editor_;
    LogBuffer log_;
    TreeModel tree_;
    std::optional<ResultTable> table_;
    std::optional<Result> lastResult_;
    History history_;
    Modal modal_ = Modal::None;
    std::optional<Panel> focusRequest_;
    int pending_ = 0;
    int resultVersion_ = 0;
    bool quitting_ = false;
    bool shutdown_ = false;
    std::function<void()> onExit_;
    Worker worker_;  // last, so it is destroyed (and joined) first
};

}  // namespace meradb::wb
