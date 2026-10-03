// cpp/src/wb_session.cpp
#include "meradb/wb_session.h"
#include "meradb/client.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "meradb/protocol.h"
#include "meradb/pytext.h"
#include <chrono>
#include <climits>
#include <cstdio>
#include <stdexcept>

namespace meradb::wb {

namespace {

constexpr const char* kDefaultHost = "127.0.0.1";
constexpr const char* kDefaultPort = "6372";

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Python's int(text) for the text of the port box (the CLI's helper, pytext::parseInt: Unicode whitespace around,
// sign, decimal digits of any script, underscores between digits). A value beyond int is clamped and left for the
// connection to reject.
int parsePort(const std::string& text) {
    const std::optional<long long> value = pytext::parseInt(text);
    if (!value) throw std::invalid_argument("invalid literal for int() with base 10: '" + text + "'");
    if (*value > INT_MAX) return INT_MAX;
    if (*value < INT_MIN) return INT_MIN;
    return static_cast<int>(*value);
}

}  // namespace

ConnectRequest makeConnectRequest(bool local, const std::string& host, const std::string& port,
                                  const std::string& password, const std::string& database) {
    ConnectRequest r;
    r.local = local;
    r.host = pytext::strip(host);
    if (r.host.empty()) r.host = kDefaultHost;
    r.port = pytext::strip(port);
    if (r.port.empty()) r.port = kDefaultPort;
    const std::string pw = pytext::strip(password);
    if (!pw.empty()) r.password = pw;
    const std::string db = pytext::strip(database);
    if (!db.empty()) r.database = db;
    return r;
}

ConnectDefaults connectDefaults(const std::string& description) {
    const ConnectDefaults fallback{kDefaultHost, kDefaultPort};
    if (pytext::startsWith(description, "local (")) return fallback;
    const std::size_t colon = description.rfind(':');
    if (colon == std::string::npos) return fallback;
    const std::string host = description.substr(0, colon);
    const std::string port = description.substr(colon + 1);
    if (host.empty() || !allDigits(port)) return fallback;
    return ConnectDefaults{host, port};
}

std::unique_ptr<Backend> defaultBackendFactory(const ConnectRequest& request, const std::string& dataDir) {
    if (request.local) return std::unique_ptr<Backend>(new LocalBackend(dataDir));
    ConnectOptions options;
    options.host = request.host;
    options.port = parsePort(request.port);
    options.password = request.password;
    options.database = request.database;
    return std::unique_ptr<Backend>(new Connection(options));
}

// ---- worker -> UI plain data ----

struct Session::Snapshot {
    std::string description;
    std::string db;
    bool inTransaction = false;
    nlohmann::ordered_json schema = nlohmann::ordered_json::array();
    std::optional<std::string> schemaError;
    bool keepHeader = false;  // the backend is gone: description / db / transaction here are not real, keep the header
};

constexpr const char* kUnknownFailure = "anjaan galti hui";

static std::string describeCurrentException() {  // inside a catch block
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return kUnknownFailure;
    }
}

struct Session::RunOutcome {
    std::vector<Result> results;
    double ms = 0;
    bool failed = false;
    std::string failure;
    std::optional<std::pair<LogKind, std::string>> notice;
    std::optional<Snapshot> snapshot;
};

struct Session::ConnectOutcome {
    std::optional<std::string> error;
    std::optional<Snapshot> snapshot;
};

// ---- construction ----

Session::Session(std::unique_ptr<Backend> backend, SessionOptions options, UiPoster poster)
    : options_(std::move(options)), post_(std::move(poster)) {
    if (!options_.factory) options_.factory = defaultBackendFactory;
    if (!options_.stamp) options_.stamp = [] { return exportFileStamp(); };
    header_.description = backend->description();
    header_.db = backend->currentDb();
    header_.inTransaction = backend->inTransaction();
    log_.addText(LogKind::Bold, "Namaste! Connected: " + header_.description);
    log_.addText(LogKind::Dim, "F1 dabao madad ke liye.");
    backend_ = std::move(backend);
    ++pending_;
    worker_.post([this] {
        auto snapshot = std::make_shared<Snapshot>();
        try {
            *snapshot = takeSnapshot();
        } catch (...) {
            *snapshot = Snapshot{};
            snapshot->keepHeader = true;
            snapshot->schemaError = describeCurrentException();
        }
        postUi([this, snapshot]() {
            --pending_;
            applySnapshot(*snapshot);
        });
    });
}

Session::~Session() { shutdown(); }

// Worker thread: hand a closure to the UI poster. The wrapper owns a copy of the alive token (not the Session), so a
// poster that delivers it after shutdown() or after the Session is gone runs a no-op and never touches `this`.
void Session::postUi(std::function<void()> fn) {
    post_([alive = alive_, fn = std::move(fn)]() {
        if (*alive) fn();
    });
}

void Session::setOnExit(std::function<void()> onExit) { onExit_ = std::move(onExit); }

// ---- state ----

std::string Session::subtitle() const {
    std::string s = header_.description + "  |  db: " + header_.db;
    if (header_.inTransaction) s += "  |  TRANSACTION (PAKKA / WAPAS)";
    return s;
}

std::string Session::busyLabel() const {
    if (quitting_) return "[band ho raha hai ...]";
    if (pending_ <= 0) return "";
    if (pending_ == 1) return "[chal raha hai]";
    return "[chal raha hai +" + std::to_string(pending_ - 1) + "]";
}

std::optional<Panel> Session::takeFocusRequest() {
    std::optional<Panel> r = focusRequest_;
    focusRequest_.reset();
    return r;
}

ConnectDefaults Session::connectDefaults() const { return meradb::wb::connectDefaults(header_.description); }

// ---- worker side ----

Session::Snapshot Session::takeSnapshot() {
    Snapshot s;
    if (!backend_) {  // never reads UI state here: applySnapshot keeps the header as it is
        s.keepHeader = true;
        s.schemaError = "backend band hai";
        return s;
    }
    s.description = backend_->description();
    s.db = backend_->currentDb();
    s.inTransaction = backend_->inTransaction();
    try {
        s.schema = backend_->schemaTree();
    } catch (const std::exception& e) {
        s.schemaError = e.what();
    }
    return s;
}

void Session::runOnWorker(const std::string& text, RunOutcome& out) {
    const auto started = std::chrono::steady_clock::now();
    try {
        if (!backend_) throw ConnectionFailed("Backend band ho chuka hai");
        out.results = backend_->runScript(text);
        out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        out.snapshot = takeSnapshot();
    } catch (...) {  // MeraDBError: the server went away; anything else is reported the same way
        out.failed = true;
        out.failure = describeCurrentException();
    }
}

// Worker thread: the echo line (and history entry) of a statement that is about to run, with the database as it is
// right now. tui.py run_text logs `current_db> text` just before running, so after an earlier ISTEMAL the prompt shows
// the new database; the closure arrives before the result closure of the same job (FIFO).
void Session::echoOnWorker(const std::string& text, bool addToHistory) {
    std::optional<std::string> db;
    try {
        if (backend_) db = backend_->currentDb();
    } catch (...) {
    }
    postUi([this, text, db, addToHistory]() {
        if (addToHistory) history_.add(text);
        log_.add(echoEntry(db ? *db : header_.db, text));
    });
}

void Session::closeBackendOnWorker() {
    if (backend_) {
        try {
            backend_->close();
        } catch (...) {
        }
        backend_.reset();
    }
}

// ---- UI side ----

void Session::enqueueRun(const std::string& text) {
    ++pending_;
    worker_.post([this, text] {
        auto out = std::make_shared<RunOutcome>();
        echoOnWorker(text, false);
        runOnWorker(text, *out);
        postUi([this, out]() {
            --pending_;
            applyRun(std::move(*out));
        });
    });
}

void Session::runText(const std::string& raw) {
    if (quitting_) return;
    const std::string text = pytext::strip(raw);
    if (text.empty()) return;
    history_.add(text);  // the echo line follows when the statement starts (echoOnWorker)
    enqueueRun(text);
}

void Session::runEditorText() { runText(editor_.runnableText()); }

void Session::showResult(Result result) {
    table_ = makeTable(result);
    lastResult_ = std::move(result);
    ++resultVersion_;
}

void Session::applySnapshot(const Snapshot& s) {
    if (!s.keepHeader) {
        header_.description = s.description;
        header_.db = s.db;
        header_.inTransaction = s.inTransaction;
    }
    if (s.schemaError) {
        log_.addText(LogKind::Dim, "(schema refresh nahi hua: " + *s.schemaError + ")");
        return;
    }
    tree_.refresh(s.schema);
}

void Session::applyRun(RunOutcome out) {
    if (out.notice) {
        log_.addText(out.notice->first, out.notice->second);
        return;
    }
    if (out.failed) {
        log_.addText(LogKind::Error, out.failure);
        log_.addText(LogKind::Dim, "Ctrl+O se dobara connect karo.");
        return;
    }
    Result* shown = nullptr;
    for (Result& r : out.results) {
        if (!r.error.empty()) {
            log_.addText(LogKind::Error, r.error);
            continue;
        }
        if (!r.columns.empty()) shown = &r;
        if (!r.message.empty()) log_.addText(LogKind::Message, r.message);
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "(%.1f ms)", out.ms);
    log_.addText(LogKind::Dim, buf);
    if (shown != nullptr) showResult(std::move(*shown));
    if (out.snapshot) applySnapshot(*out.snapshot);
}

void Session::clearLog() { log_.clear(); }

void Session::logLine(LogKind kind, const std::string& text) { log_.addText(kind, text); }

// ---- Task 7 actions ----

void Session::explainEditorText() {
    if (quitting_) return;
    const std::string text = pytext::strip(editor_.runnableText());
    ++pending_;
    worker_.post([this, text] {
        auto out = std::make_shared<RunOutcome>();
        try {
            const auto statements = parseScript(text);  // MeraDBError on bad syntax
            if (statements.size() != 1) {
                out->notice = std::make_pair(
                    LogKind::Warn, std::string("SAMJHAO ek hi query par chalta hai -- ek query select karke F6 dabao"));
            } else {
                const std::string full = "SAMJHAO " + text;
                echoOnWorker(full, true);
                runOnWorker(full, *out);
            }
        } catch (...) {
            out->notice = std::make_pair(LogKind::Error, describeCurrentException());
        }
        postUi([this, out]() {
            --pending_;
            applyRun(std::move(*out));
        });
    });
}

void Session::historyStep(int delta) {
    if (auto t = history_.step(delta)) {
        editor_.setText(*t);
        focusRequest_ = Panel::Editor;
    }
}

void Session::exportCsv() {
    if (lastResult_ == std::nullopt) {
        log_.addText(LogKind::Warn, "Pehle koi DIKHAO query chalao, phir Ctrl+S");
        return;
    }
    const ExportOutcome outcome = meradb::wb::exportCsv(*lastResult_, options_.exportBaseDir, options_.stamp());
    if (outcome.ok) {
        log_.addText(LogKind::Message,
                     std::to_string(lastResult_->rows.size()) + " row(s) CSV mein save: " + outcome.path);
    } else {
        log_.addText(LogKind::Error, "CSV save nahi hua: " + outcome.error);
    }
}

void Session::openConnectDialog() { modal_ = Modal::Connect; }

void Session::showHelp() { modal_ = Modal::Help; }

void Session::closeModal() { modal_ = Modal::None; }

void Session::connect(const ConnectRequest& request) {
    modal_ = Modal::None;
    if (quitting_) return;
    ++pending_;
    worker_.post([this, request] {
        auto out = std::make_shared<ConnectOutcome>();
        try {
            // Like tui.py, the new backend is built BEFORE the old one is touched, so a failed attempt leaves the old
            // backend (and its open transaction) exactly as it was. One case cannot follow Python: a new local engine
            // on the folder the current local engine has open would run crash recovery, which restores the snapshot
            // of a transaction that is still open here. So a local connect while a local transaction is open is
            // refused up front; PAKKA or WAPAS first. (A local engine with no transaction has nothing to recover.)
            if (request.local && dynamic_cast<LocalBackend*>(backend_.get()) != nullptr && backend_->inTransaction())
                throw std::runtime_error("ek transaction khula hai -- pehle PAKKA ya WAPAS karo, phir Local mode");
            std::unique_ptr<Backend> fresh = options_.factory(request, options_.dataDir);  // may throw
            std::unique_ptr<Backend> old = std::move(backend_);
            backend_ = std::move(fresh);
            try {
                if (old) old->close();  // rolls an open transaction back (a remote one is the server's to undo)
            } catch (...) {
            }
            out->snapshot = takeSnapshot();
        } catch (...) {
            out->error = describeCurrentException();
        }
        postUi([this, out]() {
            --pending_;
            applyConnect(*out);
        });
    });
}

void Session::applyConnect(const ConnectOutcome& out) {
    if (out.error) {
        log_.addText(LogKind::Error, "Connect nahi hua: " + *out.error);
        return;
    }
    log_.addText(LogKind::Connected, "Connected: " + out.snapshot->description);
    applySnapshot(*out.snapshot);
}

void Session::activateTreeRow(int row) {
    const TreeActivation act = tree_.activate(row, header_.db);
    for (const std::string& script : act.scripts) runText(script);
    if (!act.insertText.empty()) {
        editor_.insert(act.insertText);
        focusRequest_ = Panel::Editor;
    }
}

void Session::requestQuit() {
    if (quitting_ || shutdown_) return;
    quitting_ = true;
    pending_ -= static_cast<int>(worker_.cancelPending());  // those jobs will never post back
    worker_.post([this] {
        closeBackendOnWorker();
        postUi([this] {
            if (onExit_) onExit_();
        });
    });
}

void Session::shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    if (!quitting_) {
        quitting_ = true;
        worker_.cancelPending();
        worker_.post([this] { closeBackendOnWorker(); });
    }
    worker_.stopAndJoin();
    *alive_ = false;  // closures still queued in the poster become no-ops
}

}  // namespace meradb::wb
