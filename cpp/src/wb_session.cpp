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

// Python's int(text) for the text of the port box: optional sign, ASCII digits.
int parsePort(const std::string& text) {
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    const std::string digits = text.substr(i);
    if (!allDigits(digits)) throw std::invalid_argument("invalid literal for int() with base 10: '" + text + "'");
    long long value = 0;
    for (char c : digits) {
        value = value * 10 + (c - '0');
        if (value > INT_MAX) {
            value = INT_MAX;
            break;
        }
    }
    return static_cast<int>(negative ? -value : value);
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
};

struct Session::RunOutcome {
    std::vector<Result> results;
    double ms = 0;
    bool failed = false;
    std::string failure;
    std::optional<std::string> echo;  // text to put in history and the log when the job completes (explain)
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
        Snapshot snapshot = takeSnapshot();
        postUi([this, snapshot]() {
            --pending_;
            applySnapshot(snapshot);
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
    if (!backend_) {
        s.description = header_.description;
        s.db = header_.db;
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
    } catch (const std::exception& e) {  // MeraDBError: the server went away; anything else is reported the same way
        out.failed = true;
        out.failure = e.what();
    }
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
        RunOutcome out;
        runOnWorker(text, out);
        postUi([this, out]() mutable {
            --pending_;
            applyRun(std::move(out));
        });
    });
}

void Session::runText(const std::string& raw) {
    if (quitting_) return;
    const std::string text = pytext::strip(raw);
    if (text.empty()) return;
    history_.add(text);
    log_.add(echoEntry(header_.db, text));
    enqueueRun(text);
}

void Session::runEditorText() { runText(editor_.runnableText()); }

void Session::showResult(const Result& result) {
    lastResult_ = result;
    table_ = makeTable(result);
    ++resultVersion_;
}

void Session::applySnapshot(const Snapshot& s) {
    header_.description = s.description;
    header_.db = s.db;
    header_.inTransaction = s.inTransaction;
    if (s.schemaError) {
        log_.addText(LogKind::Dim, "(schema refresh nahi hua: " + *s.schemaError + ")");
        return;
    }
    tree_.refresh(s.schema);
}

void Session::applyRun(RunOutcome out) {
    if (out.echo) {
        history_.add(*out.echo);
        log_.add(echoEntry(header_.db, *out.echo));
    }
    if (out.notice) {
        log_.addText(out.notice->first, out.notice->second);
        return;
    }
    if (out.failed) {
        log_.addText(LogKind::Error, out.failure);
        log_.addText(LogKind::Dim, "Ctrl+O se dobara connect karo.");
        return;
    }
    const Result* shown = nullptr;
    for (const Result& r : out.results) {
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
    if (shown != nullptr) showResult(*shown);
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
        RunOutcome out;
        try {
            const auto statements = parseScript(text);  // MeraDBError on bad syntax
            if (statements.size() != 1) {
                out.notice = std::make_pair(
                    LogKind::Warn, std::string("SAMJHAO ek hi query par chalta hai -- ek query select karke F6 dabao"));
            } else {
                const std::string full = "SAMJHAO " + text;
                out.echo = full;
                runOnWorker(full, out);
            }
        } catch (const std::exception& e) {
            out.notice = std::make_pair(LogKind::Error, std::string(e.what()));
        }
        postUi([this, out]() mutable {
            --pending_;
            applyRun(std::move(out));
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
        ConnectOutcome out;
        try {
            if (request.local && dynamic_cast<LocalBackend*>(backend_.get()) != nullptr) {
                // R14: never two engines on one folder. The old engine is closed (rolling its transaction back).
                backend_->close();
            }
            std::unique_ptr<Backend> fresh = options_.factory(request, options_.dataDir);  // may throw
            std::unique_ptr<Backend> old = std::move(backend_);
            backend_ = std::move(fresh);
            try {
                if (old) old->close();
            } catch (...) {
            }
            out.snapshot = takeSnapshot();
        } catch (const std::exception& e) {
            out.error = e.what();
        }
        postUi([this, out]() {
            --pending_;
            applyConnect(out);
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
