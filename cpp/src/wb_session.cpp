// cpp/src/wb_session.cpp
#include "meradb/wb_session.h"
#include "meradb/client.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "meradb/protocol.h"
#include "meradb/pytext.h"
#include "meradb/tokenizer.h"
#include <algorithm>
#include <chrono>
#include <cctype>
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
                                  const std::string& password, const std::string& database,
                                  const std::string& user, const std::string& tls) {
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
    const std::string name = pytext::strip(user);
    if (!name.empty()) r.user = name;
    // The TLS field: empty = plain, "on" = TLS trusting the computer's roots, anything else = the certificate file to trust.
    const std::string tlsText = pytext::strip(tls);
    r.tls = !tlsText.empty();
    if (r.tls && tlsText != "on") r.tlsCaFile = tlsText;
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

std::string statementAt(const std::string& script, std::size_t offset) {
    std::vector<Token> tokens;
    try {
        tokens = tokenize(script);
    } catch (...) {
        return script;
    }
    const auto parses = [](const std::string& piece) {
        try {
            return parseScript(piece).size() == 1;
        } catch (...) {
            return false;
        }
    };
    constexpr int kMostSemicolons = 256;   // of one statement: a procedure body; a broken text stops looking here
    std::string last;
    std::size_t i = 0;
    while (i < tokens.size() && tokens[i].type != TokenType::Eof) {
        const std::size_t start = static_cast<std::size_t>(tokens[i].start);
        std::size_t end = std::string::npos, firstSemicolon = std::string::npos, j = i;
        int tried = 0;
        for (; tokens[j].type != TokenType::Eof; ++j) {
            if (tokens[j].type != TokenType::Symbol || tokens[j].textValue != ";") continue;
            if (firstSemicolon == std::string::npos) firstSemicolon = j;
            if (parses(script.substr(start, static_cast<std::size_t>(tokens[j].end) - start))) {
                end = j;
                break;
            }
            if (++tried >= kMostSemicolons) break;
        }
        if (end == std::string::npos) end = firstSemicolon != std::string::npos ? firstSemicolon : j - 1;   // to report its error
        const std::size_t stop = static_cast<std::size_t>(tokens[end].end);
        last = script.substr(start, stop - start);
        if (offset <= stop) return last;
        i = end + 1;
    }
    return last;
}

std::unique_ptr<Backend> defaultBackendFactory(const ConnectRequest& request, const std::string& dataDir) {
    if (request.local) return std::unique_ptr<Backend>(new LocalBackend(dataDir));
    ConnectOptions options;
    options.host = request.host;
    options.port = parsePort(request.port);
    options.password = request.password;
    options.database = request.database;
    options.user = request.user;
    options.tls = request.tls;
    options.tlsCaFile = request.tlsCaFile;
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

// "ISTEMAL x;" and nothing else: a database switch is not a query to go back to. A script that starts with one is.
static bool isBareSwitch(const std::string& raw) {
    std::string text = pytext::strip(raw);
    if (text.size() < 8) return false;
    std::string head = text.substr(0, 8);
    for (char& ch : head) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (head != "ISTEMAL ") return false;
    if (text.back() == ';') text.pop_back();
    return text.find_first_of(";\n") == std::string::npos;
}

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
    std::string text;      // what was run
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
    if (options_.keepWork && header_.description.rfind("local", 0) == 0) {   // a server connection keeps no history here
        store_ = QueryStore(options_.dataDir);
        reloadSaved();
        scriptDb_ = header_.db;
        seedHistory();
        lastScript_ = scriptDb_.empty() ? std::string() : store_.loadScript(scriptDb_);
        if (!lastScript_.empty()) editor_.setText(lastScript_);
        lastScript_ = editor_.text();
    }
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
        const std::string where = db ? *db : header_.db;
        log_.add(echoEntry(where, text));
        if (store_.enabled() && !where.empty() && !isBareSwitch(text)) {
            SavedQuery q{text, QueryStore::nowStamp()};
            store_.append(where, q);
            if (where == savedDb_) {
                // queued behind a database switch: that switch brought this database's history after runText added it
                if (history_.size() == 0 || history_.items().back() != text) history_.add(text);
                saved_.push_back(std::move(q));
                ++savedVersion_;
            }
        }
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
        out->text = text;
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
    // Running the entry on screen is still only a look at an old query: the script stays kept, Agli brings it back.
    if (stepping_ && editor_.text() != steppedText_) stepping_ = false;
    switchScriptTo_.clear();   // a card's ISTEMAL that failed must not swap the writing area on a later typed ISTEMAL
    enqueueRun(text);
}

void Session::runEditorText() { runText(editor_.runnableText()); }

void Session::runEditorStatement() {
    if (editor_.hasSelection()) {
        runEditorText();
        return;
    }
    const Pos caret = editor_.cursor();
    std::size_t offset = editor_.byteOffset(caret.row, caret.col);
    for (int row = 0; row < caret.row; ++row) offset += editor_.line(row).size() + 1;
    runText(statementAt(editor_.text(), offset));
}

void Session::addQueryToEditor(const std::string& text) {
    endWalk();
    const std::string current = editor_.text();
    const bool blank = pytext::strip(current).empty();
    editor_.setText(blank ? text : current + (current.back() == '\n' ? "\n" : "\n\n") + text);
    if (blank) {
        editor_.selectAll();
    } else {
        const int rows = 1 + static_cast<int>(std::count(text.begin(), text.end(), '\n'));
        const Pos end = editor_.cursor();
        editor_.selectRange(Pos{end.row - rows + 1, 0}, end);
    }
    focusRequest_ = Panel::Editor;
}

void Session::selectResultSet(int index) {
    if (index < 0 || index >= static_cast<int>(resultSets_.size()) || index == currentSet_) return;
    currentSet_ = index;
    ++resultVersion_;
}

void Session::reloadSaved() {
    savedDb_ = header_.db;
    saved_ = savedDb_.empty() ? std::vector<SavedQuery>() : store_.load(savedDb_);

    ++savedVersion_;
}

void Session::persistEditor(bool force) {
    if (!store_.enabled() || scriptDb_.empty()) return;
    const std::string text = editor_.text();
    if (text == lastScript_) return;
    // A history entry shown by Pichli / Agli is a look at an old query, not an edit of the script: it is only saved once
    // the user changes it. Agli past the newest entry brings the script back.
    if (stepping_ && text == steppedText_) return;
    stepping_ = false;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - lastScriptWrite_ < std::chrono::milliseconds(700)) return;
    store_.saveScript(scriptDb_, text);
    lastScript_ = text;
    lastScriptWrite_ = now;
}

void Session::openDatabase(const std::string& asTyped) {
    std::string name = asTyped;   // the engine folds a name to lower case; the answer is compared with this
    for (char& ch : name)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    if (name.empty()) return;
    if (name == header_.db) return;
    persistEditor(true);
    switchScriptTo_ = name;
    if (quitting_) return;
    enqueueRun("ISTEMAL " + name + ";");   // not added to the history: it is not a query to go back to
}

// Pichli / Agli walk through the queries saved for the database: from the start, and whenever the database changes.
void Session::seedHistory() {
    history_ = History();
    for (const SavedQuery& q : saved_) history_.add(q.text);
}

void Session::clearSavedQueries() {
    if (!store_.enabled() || savedDb_.empty()) return;
    store_.clear(savedDb_);
    saved_.clear();
    ++savedVersion_;
    endWalk();            // an entry on screen has just been deleted: the script comes back
    seedHistory();
}

void Session::endWalk() {
    if (stepping_ && editor_.text() == steppedText_) editor_.setText(draft_);
    stepping_ = false;
}

void Session::applySnapshot(const Snapshot& s) {
    if (!s.keepHeader) {
        header_.description = s.description;
        header_.db = s.db;
        header_.inTransaction = s.inTransaction;
        if (store_.enabled() && header_.db != savedDb_) {   // the Queries tab and Pichli show the same database, always
            reloadSaved();
            seedHistory();
        }
        if (store_.enabled() && !switchScriptTo_.empty() && switchScriptTo_ == header_.db) {   // opened from the Home cards
            switchScriptTo_.clear();
            persistEditor(true);   // the last edits of the database being left, inside the save delay
            seedHistory();
            stepping_ = false;     // the walk belonged to the database being left
            scriptDb_ = header_.db;
            editor_.setText(store_.loadScript(scriptDb_));
            lastScript_ = editor_.text();
        }
    }
    if (s.schemaError) {
        log_.addText(LogKind::Dim, "(schema refresh nahi hua: " + *s.schemaError + ")");
        return;
    }
    tree_.refresh(s.schema);
}

void Session::applyRun(RunOutcome out) {
    runErrors_.clear();
    if (out.notice) {
        log_.addText(out.notice->first, out.notice->second);
        if (out.notice->first == LogKind::Error) runErrors_.push_back({0, out.notice->second});
        return;
    }
    if (out.failed) {
        log_.addText(LogKind::Error, out.failure);
        log_.addText(LogKind::Dim, "Ctrl+O se dobara connect karo.");
        runErrors_.push_back({0, out.failure});
        return;
    }
    std::vector<ResultSet> sets;
    const bool alone = out.results.size() == 1;
    for (std::size_t i = 0; i < out.results.size(); ++i) {
        Result& r = out.results[i];
        const int statement = static_cast<int>(i) + 1;
        if (!r.error.empty()) {
            log_.addText(LogKind::Error, r.error);
            runErrors_.push_back({alone ? 0 : statement, r.error});
            continue;
        }
        if (!r.message.empty()) log_.addText(LogKind::Message, r.message);
        if (!r.columns.empty()) {
            ResultSet set;
            set.statement = statement;
            set.table = makeTable(r);
            set.result = std::move(r);
            sets.push_back(std::move(set));
        }
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "(%.1f ms)", out.ms);
    log_.addText(LogKind::Dim, buf);
    if (!sets.empty()) {   // a run without a table leaves the last one on screen
        resultSets_ = std::move(sets);
        currentSet_ = static_cast<int>(resultSets_.size()) - 1;
        ++resultVersion_;
    }
    const std::string dbBefore = savedDb_;
    if (out.snapshot) applySnapshot(*out.snapshot);
    // a script that moved to another database was saved under the old one, but it was just run: Pichli still reaches it
    if (store_.enabled() && savedDb_ != dbBefore && !isBareSwitch(out.text)) history_.add(out.text);
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

// Pichli / Agli. The text that was in the writing area before the walk is kept as a draft: Agli past the newest entry
// puts it back instead of leaving the area empty (so a script is never lost by looking at an old query). An entry equal
// to the text on screen is passed over, or the press would change nothing and look broken. keepWork = false follows
// tui.py exactly (the workbench_diff probe compares against it).
void Session::historyStep(int delta) {
    if (!options_.keepWork) {
        if (auto t = history_.step(delta)) {
            editor_.setText(*t);
            focusRequest_ = Panel::Editor;
        }
        return;
    }
    if (history_.size() == 0) return;
    const std::string current = editor_.text();
    if (stepping_ && current != steppedText_) stepping_ = false;   // the entry was edited: that is the new draft
    if (!stepping_) {
        if (delta > 0 && history_.position() >= history_.size()) return;   // nothing newer and nothing to bring back
        persistEditor(true);   // the draft may be younger than the save delay, and the walk does not save
        draft_ = current;
        stepping_ = true;
    }
    const std::string shown = pytext::strip(current);
    std::optional<std::string> entry = history_.step(delta);
    while (!entry->empty() && *entry == shown && (delta > 0 || history_.position() > 0)) entry = history_.step(delta);
    focusRequest_ = Panel::Editor;
    if (entry->empty()) {   // past the newest entry
        editor_.setText(draft_);
        stepping_ = false;
    } else {
        if (*entry != shown) editor_.setText(*entry);
        steppedText_ = editor_.text();
    }
}

std::string Session::historyLabel() const {
    if (!stepping_ || history_.position() >= history_.size()) return "";
    return std::to_string(history_.position() + 1) + "/" + std::to_string(history_.size());
}

void Session::exportCsv() {
    const Result* shown = lastResult();
    if (shown == nullptr) {
        log_.addText(LogKind::Warn, "Pehle koi DIKHAO query chalao, phir Ctrl+S");
        return;
    }
    const ExportOutcome outcome = meradb::wb::exportCsv(*shown, options_.exportBaseDir, options_.stamp());
    if (outcome.ok) {
        const std::string message = std::to_string(shown->rows.size()) + " row(s) CSV mein save: " + outcome.path;
        log_.addText(LogKind::Message, message);
        lastExport_.serial += 1;
        lastExport_.message = message;
        lastExport_.path = outcome.path;
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
    // The queries and the script are kept in the local folder, for local mode only (see the constructor).
    const bool keep = options_.keepWork && !options_.dataDir.empty() && out.snapshot->description.rfind("local", 0) == 0;
    if (keep != store_.enabled()) {
        endWalk();
        if (!keep) persistEditor(true);
        store_ = QueryStore(keep ? options_.dataDir : std::string());
        saved_.clear();
        savedDb_.clear();    // back in local mode, applySnapshot below loads the list and the history
        ++savedVersion_;
        history_ = History();
        scriptDb_ = keep ? out.snapshot->db : std::string();
        if (keep && !scriptDb_.empty() && pytext::strip(editor_.text()).empty()) editor_.setText(store_.loadScript(scriptDb_));
        lastScript_ = editor_.text();   // text brought from the server is saved only once it changes
    }
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
    persistEditor(true);
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
    persistEditor(true);
    if (!quitting_) {
        quitting_ = true;
        worker_.cancelPending();
        worker_.post([this] { closeBackendOnWorker(); });
    }
    worker_.stopAndJoin();
    *alive_ = false;  // closures still queued in the poster become no-ops
}

}  // namespace meradb::wb
