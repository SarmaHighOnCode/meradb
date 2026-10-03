# MeraDB C++ Port — Phase 4: Workbench Implementation Plan

**Goal:** Give the C++ command line the full-screen workbench: `meradb_cli workbench` (also `tui` and the
`--tui` shortcut) opens the same screen as `python -m meradb workbench` (meradb/tui.py): header with
`host:port | db | TRANSACTION`, schema tree, results table, log, multi-line syntax-highlighted query editor,
footer, query history, CSV export, connect dialog, help screen. Every key binding, Hinglish label and
message of `tui.py` is kept. Statements run on a dedicated worker thread so the screen never freezes.

**Architecture:** The workbench is split like the shell was: a **pure model** (no terminal, no FTXUI
drawing) holds all state and decisions, a thin **FTXUI view** draws it and turns key events into model
calls. The model's `Session` owns the `Backend` and ONE worker thread; every database call of the session
(statements, schema refresh, connecting, closing) runs on that thread, and results come back to the UI
thread as closures posted through a `UiPoster` (`ScreenInteractive::Post` in the real program, a manual queue
in tests). So the whole workbench, including threading, is testable without a terminal: unit tests on the
model, a headless end-to-end test that renders the real component tree to an FTXUI `Screen` and feeds it
synthetic events, and a comparison against the Python workbench driven by Textual's `Pilot`.

**Tech Stack:** C++17, CMake, Catch2 v3 (present), nlohmann/json (present). **One new dependency:**
[FTXUI](https://github.com/ArthurSonzogni/FTXUI) v5.0.0, fetched with `FetchContent` exactly like the other
two, statically linked, behind the CMake option `MERADB_WORKBENCH` (default ON). Python 3 with Textual 8.x is
used only by the comparison script and the golden generator, and both skip cleanly without it.

**Spec:** [docs/cpp-port/specs/2026-09-27-cpp-port-design.md](../specs/2026-09-27-cpp-port-design.md)
(Phase 4 row: "FTXUI-based full-screen client matching `tui.py`'s panels (schema tree, query editor, results
table, syntax highlighting)"; its risk note says the TUI may slip without blocking submission, so the option
to build without it must stay healthy).

**Starts from:** `main` at or after `4cdbc6c` (Phases 1-3 complete and merged). Create the Phase 4 work on a
new branch off `main`.

**Out of scope (deliberately left to the project owner):** the three prove-it exercises in
`docs/ROADMAP.md` — the `^` power operator, the `.hexdump <table>` shell command, and `GINO(ALAG x)`. Do
**not** add them. Also out of scope: Phase 5 (final report and polish), a history file (the Python
workbench keeps history in memory only), any change under `meradb/` or `examples/`.

## Global Constraints

Everything in the Phase 1-3 plans' Global Constraints still applies. Additions and the lessons that recurred
in earlier reviews:

- **Python is the oracle, never the plan text.** Labels, messages, colours, ordering of log lines and
  the quirks of `tui.py` come from reading `meradb/tui.py` / `meradb/highlight.py` and from *running* them
  (Textual's `Pilot` is installed on the dev machine: `python -c "import textual"`; the repo pins 8.x). Where
  this plan and Python disagree, Python wins: fix the plan's test, not the workbench. One known case: the
  results title is `Results -- N row(s)` with two ASCII hyphens (as in the source), not an em dash.
- **GCC evaluates call arguments right-to-left.** Never put two calls that can throw or have side effects
  in one argument list, one braced initializer or one `<<` chain. Bind child nodes and intermediate results
  into named locals in source order first. This bites FTXUI code especially: `hbox(a(), b())` with stateful
  `a()` / `b()` renders in the wrong order on MinGW. Build `Elements` vectors with `push_back` in order.
- **MSVC / portability rules** (only MinGW g++ is built on the dev machine, so review by reading): no
  `long double`, `__int128`, VLAs, `ssize_t`, GCC builtins, or POSIX-only headers outside `#ifdef`
  blocks; no `std::getenv` / `localtime` / `strerror` (use `sys::`); never include `<windows.h>` from a
  header, and only `sys_compat.cpp` includes it (`WIN32_LEAN_AND_MEAN` and `NOMINMAX` are defined by the
  build and propagate to the workbench library); no `std::filesystem::path` built from a UTF-8 `std::string`
  without `u8path`; sources containing UTF-8 rely on the existing `/utf-8` flag.
- **C++17 only.** No designated initializers, no `operator==(...) = default`, no `std::format`, no
  `<span>`, no concepts. FTXUI v5 itself is C++17.
- **Tests never touch the real data folder or fixed ports.** Every test that opens a database uses a temp
  folder (`TempDir`, or `MERADB_DATA=<tmp>` / `--data <tmp>`); servers bind port 0; every Python script
  removes `MERADB_*` from the child environment, runs with a temp current directory (the workbench writes
  `./exports/`), and leaves nothing behind.
- **No terminal in CI.** Everything except the terminal-mode guard and the real key bytes a terminal sends
  is tested headlessly; those two are covered by the manual checklist (Task 14) and the key-probe tool
  (Task 1).
- **Windows MinGW builds need PATH exported in every shell**: CMake's `bin` and WinLibs' `mingw64\bin`, e.g.
  `export PATH="/c/Program Files/CMake/bin:<winlibs>/mingw64/bin:$PATH"`.
- **Hygiene:** no attribution trailers and no remarks about how the code was produced anywhere in code, comments,
  docs or commit messages; no personal details; commit messages are plain one-liners with no
  trailer lines. Never modify `meradb/` or `examples/` (`git diff <base> -- meradb
  examples` must be empty).
- **Deliberate divergences** from Python are allowed only when listed in "Rulings" below; each is copied into
  `docs/CPP.md` in Task 14.

---

## Rulings / decisions

| # | Ruling | Why |
|---|---|---|
| R1 | **FTXUI v5.0.0**, `URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v5.0.0.tar.gz` (tag verified to exist; released August 2024), built with `FTXUI_BUILD_EXAMPLES/TESTS/DOCS` and `FTXUI_ENABLE_INSTALL` OFF, targets `ftxui::screen ftxui::dom ftxui::component`. CMake option `MERADB_WORKBENCH` (default ON). OFF = today's stub, nothing is fetched. | Same pinned-tarball style as nlohmann/json and Catch2. The option keeps an offline checkout buildable (`-DMERADB_WORKBENCH=OFF`, or `-DFETCHCONTENT_SOURCE_DIR_FTXUI=<unpacked tarball>`). |
| R2 | The query editor is **our own component** over a pure `TextBuffer`, not `ftxui::Input`. | `Input` has no per-token colours (its `transform` receives an already-rendered element), no selection, no line numbers. A buffer we own gives highlighting, selection-aware run, line numbers and unit tests for free. Cost: about 250 lines. |
| R3 | **Selection** (so F5 "runs the selection" as in Python): `Shift`+arrows / Home / End and `Ctrl+A` (select all). No mouse selection, no clipboard commands (the terminal's own paste works: it arrives as typed characters). | `TextArea` in Textual also supports mouse selection and copy/paste actions; those are not reproduced. Listed as a divergence. Shift+arrow sequences are matched on the raw event text (`ESC [ 1 ; 2 A`..); Task 1's key probe confirms them per terminal. |
| R4 | **Editor and log layout:** the editor scrolls horizontally (no soft wrap); the log wraps by characters, not words. | Textual's `TextArea` soft-wraps and `RichLog` wraps at words. Horizontal scroll keeps cursor arithmetic trivial; character wrap is exact and testable. Divergences. |
| R5 | **Threading:** one `Worker` thread per `Session`, FIFO queue, never a pool. Only the worker touches the `Backend`. All other state (log, tree, table, header, history, editor) is owned by the UI thread and changed only by closures posted back through `UiPoster`. | A transaction's statements must run on the thread that ran `SHURU` (the engine's lock is re-entrant per thread). |
| R6 | **Keys pressed while a statement runs** are accepted and queued in order (`F5` again queues the second run). A busy label shows `[chal raha hai]` / `[chal raha hai +N]`. Python blocks its UI instead, with the same effect on ordering. | UI thread must never block on the database. |
| R7 | **Ctrl+Q while a statement runs:** queued-but-not-started jobs are dropped, the running statement finishes, the backend is closed on the worker (rolling back an open transaction), then the loop exits. The header says `[band ho raha hai ...]` meanwhile. A second Ctrl+Q does nothing. | A statement is never abandoned half-way (same reasoning as the shell's Ctrl+C, Phase 3 D5). |
| R8 | **Ctrl+C never kills the workbench.** Terminal modes that turn keys into signals or flow control are switched off while it runs (`ISIG`, `IXON`, `IEXTEN` on POSIX; `ENABLE_PROCESSED_INPUT` on Windows), so Ctrl+C, Ctrl+S, Ctrl+Q, Ctrl+O arrive as ordinary keys. Ctrl+C logs `Bahar niklne ke liye Ctrl+Q dabao.` (yellow), as Textual only hints at Ctrl+Q. If a signal still ends FTXUI's loop (SIGTERM, hang-up, a terminal that ignores the mode change), the post-loop code waits for the worker and closes the backend, so an open transaction is rolled back cleanly. A hard kill, a closed window (`CTRL_CLOSE_EVENT`) or Ctrl+Break leaves it to crash recovery at the next start, as Python does. | Ctrl+S without this freezes the terminal on POSIX (XOFF). FTXUI v5.0.0's own SIGINT handler exits the loop. |
| R9 | **Help text** = `KEYS_HELP` (verbatim from `tui.py`, plus a short "extra keys" section) + `docs/LANGUAGE.md`, **embedded at build time** (a CMake script turns the file into a byte array; a missing file embeds the fallback `*(docs/LANGUAGE.md nahi mila)*`). A tiny Markdown subset is rendered by our own code. | A built binary has no reliable path to the repo. Editing the file needs a rebuild: divergence. |
| R10 | **Colours** use the Dracula RGB values of `highlight.RICH_STYLES` and `tui.py` (`#ff79c6` keyword, `#f1fa8c` string, `#bd93f9` number, `#6272a4` comment, `#8be9fd` type, `#50fa7b` function / messages, `#ffb86c` PK / UQ). FTXUI down-converts to 256 / 16 colours on weaker terminals. Errors are `#ff5555`, warnings `#f1fa8c`. The editor uses the same table as the log echo (`RICH_STYLES`), not Textual's theme table. | One palette, one place. |
| R11 | **Highlighting** is a lenient hand-written scanner equivalent to `highlight.py`'s regex (the strict tokenizer would throw on `'unterminated`), sharing the keyword list with the real tokenizer through a new `isKeyword()` accessor. Python's `\d` and `\b` are Unicode-aware; the scanner treats ASCII digits as digits and every non-ASCII byte as a word character. Divergence, golden-tested on every line of both example scripts plus edge lines. | Same reason as Python: the highlighter must never fail while the user types. |
| R12 | **Extra key aliases** beyond Python: `Ctrl+P` / `Ctrl+N` (history, because Ctrl+Up / Ctrl+Down are not delivered by every terminal), `Ctrl+A` (select all), `PageUp/PageDown`, mouse click to focus a panel and the wheel to scroll (optional last step of Task 11). | Harmless additions; listed in the help screen's extra section. |
| R13 | **CSV** follows Python's `csv.writer` defaults exactly (`\r\n` line ends, minimal quoting, a lone empty field in a one-column row is written `""`), UTF-8 without BOM, `exports/meradb-YYYYmmdd-HHMMSS.csv` relative to the current directory. A write failure is logged in red (`CSV save nahi hua: <reason>`); Python would crash with a traceback. | Parity where it is visible, robustness where Python has none. |
| R14 | **Tree:** the selected row is kept by node key across a refresh (Python resets to the top after every query). Guide lines are replaced by `▼` / `▶` markers. Switching to local mode while the current backend is *also* a local engine closes the old one first (rolls its transaction back), because two in-process engines on one folder would let the second one's crash recovery undo the first one's open transaction. | Usability / data safety; both small. |
| R15 | **Explain (F6)** parses on the worker thread (a pasted megabyte must not freeze the screen, and the parser's stack budget is per thread). History and the echo line for `SAMJHAO ...` are added when the job completes. `F5` echoes the statement at once. | Responsiveness. |
| R16 | Cell text: `\n` and `\r` inside a value are shown as `↵`, a tab as a space (a table row is one line). CSV keeps the raw text. The log keeps at most 20,000 lines (oldest dropped). | Table rows are one line; unbounded memory is not a feature. |
| R17 | `meradb workbench` needs a terminal on stdin and stdout: otherwise one line on stderr (`Workbench ke liye terminal chahiye ...`) and exit 1, before anything is opened. FTXUI is statically linked, so Python's "install Textual" path does not exist. With `MERADB_WORKBENCH=OFF` the old note (`abhi C++ version mein nahi hai`) is printed and the exit code is 1. Normal exit code is 0. | Piped input cannot drive a full-screen UI. |
| R18 | The CLI reaches the workbench through a **function-pointer hook** (`setWorkbenchRunner`), set in `main.cpp`. | `meradb_core` (which holds `cli.cpp`) must not depend on the FTXUI library; the hook keeps the dependency one-way and lets tests call `cliMain({"workbench"})` with a fake runner. |

---

## Reference: what the Python workbench does (meradb/tui.py, read and confirmed with Textual's Pilot)

```
Layout   Header (title "MeraDB Workbench", sub title "<description>  |  db: <db>[  |  TRANSACTION (PAKKA / WAPAS)]")
         Horizontal: Tree "Schema" (width 32) | Vertical: DataTable "Results" (1fr), RichLog "Log" (height 10),
         QueryEditor "Query  [F5 = chalao, F6 = samjhao]" (height 9)      Footer (bindings)
Focus    Tab order = DOM order = schema tree, results, log, editor; start in the editor; the focused
         panel's border turns heavy yellow.
Start    log: bold "Namaste! Connected: <description>", dim "F1 dabao madad ke liye."; schema loaded; header set.
Keys     F5 / Ctrl+R run (selected text if any, else the whole editor)   F6 explain
         Ctrl+Up / Ctrl+Down history   Ctrl+S CSV   Ctrl+O connect   Ctrl+L clear log   F1 help   Ctrl+Q quit
         (priority bindings: they work while the editor has focus; Ctrl+L and F1 do not need to)
run_text(text)   text = text.strip(); empty -> nothing at all (no history, no echo).
         history: append unless equal to the last entry; position = len(history).
         log (dim) "<db>> " + highlighted text (text.strip().splitlines(), spans from highlight.py)
         results = backend.run_script(text)   [MeraDBError -> bold red str(e), dim "Ctrl+O se dobara connect karo.", STOP:
                                               no ms line, no schema refresh, no header update]
         for each result: error -> bold red line; columns -> remembered as the table to show (the LAST one);
                          message (non-empty) -> "#50fa7b" line
         dim "(<ms:.1f> ms)"  ; table shown if any ; refresh schema ; update header
refresh_schema   schema_tree() error -> dim "(schema refresh nahi hua: <e>)" and the tree is left alone.
         Else the tree is rebuilt: root "Databases" expanded; a database node is expanded if it is the current one
         OR was expanded before; a table node only if it was expanded before. Labels: current db bold #50fa7b,
         table bold, column "<name> <type lower, #8be9fd>[ PK|UQ #ffb86c][ NN dim]" (PK wins over UQ).
Tree Enter  (Textual also toggles the node's expansion)  column -> insert its name at the editor cursor, focus editor;
         database/table: if db != current db -> run_text("ISTEMAL <db>;"); table -> run_text("DIKHAO * SE <t> SIRF 100;")
cell(value)  None -> "KHALI" dim italic; bool -> SACH/JHOOTH green/red; int/float -> format_value, #bd93f9, right-aligned;
         else format_value. Results title "Results -- <n> row(s)". Zebra stripes, row cursor.
F6       text = editor text (selection-aware).strip(); parse(text): MeraDBError -> bold red str(e) and stop;
         len != 1 -> yellow "SAMJHAO ek hi query par chalta hai -- ek query select karke F6 dabao";
         else run_text("SAMJHAO " + text).
History  empty -> nothing; pos = clamp(pos + step, 0, len); text = history[pos] or "" at len; editor text replaced,
         cursor to the end, editor focused.
Ctrl+S   no table yet -> yellow "Pehle koi DIKHAO query chalao, phir Ctrl+S"; else exports/meradb-%Y%m%d-%H%M%S.csv
         (csv.writer defaults, header row, None -> ""), green "<n> row(s) CSV mein save: <absolute path>".
Ctrl+O   dialog: Host (backend's host, default 127.0.0.1), Port (backend's port, default 6372), Password (hidden),
         Database (optional); buttons Connect / Local mode / Cancel; Enter in a field = Connect; Esc = Cancel.
         choice: values stripped; host/port defaults when empty; password / database None when empty.
         Local mode -> Engine(data_dir) (data_dir = the --data option); Connect -> Connection(host, int(port), password, database).
         Failure (MeraDBError or ValueError) -> bold red "Connect nahi hua: <e>", nothing changes.
         Success: the new backend replaces the old, old.close(), bold #50fa7b "Connected: <description>", schema + header refresh.
F1       help modal: KEYS_HELP + docs/LANGUAGE.md ("*(docs/LANGUAGE.md nahi mila)*" if unreadable); Esc / F1 / q close it.
Exit     the backend is closed (an unfinished transaction is rolled back); exit code 0.
```

## Module map

| Python | C++ (new unless noted) |
|---|---|
| `highlight.py` | `highlight.h/.cpp` in `meradb_core` (+ `isKeyword` in `tokenizer`, `isAggregateName` in `aggregates`) |
| `tui.py` cell(), highlighted(), CSV, history, log echo | `wb_text.h/.cpp` (styles, cells, CSV, echo lines, clipping, wrapping, history, scroll state) |
| `QueryEditor` (TextArea) | `wb_editor.h/.cpp` (`TextBuffer`, editor row rendering data) |
| `Tree` handling, `refresh_schema` | `wb_tree.h/.cpp` (`TreeModel`) |
| `app.run_text`, actions, `_switch_backend` | `wb_worker.h/.cpp` (`Worker`), `wb_session.h/.cpp` (`Session`) |
| `HelpScreen`, `KEYS_HELP`, Markdown widget | `wb_help.h/.cpp`, `wb_markdown.h/.cpp`, `cmake/embed_file.cmake` |
| Textual widgets and CSS | `wb_ui.h/.cpp`, `wb_panels.cpp`, `wb_dialogs.cpp` (FTXUI components) |
| `run_workbench` | `workbench.h/.cpp` (`runWorkbench`, `UiBridge`) |
| `cmd_workbench` | `cli.cpp` hook + `main.cpp` |
| terminal modes | `sys::TerminalModeGuard` in `sys_compat` |

The model files (`wb_*.cpp` except `wb_ui.cpp`, `wb_panels.cpp`, `wb_dialogs.cpp`, `workbench.cpp`) never draw. They use
FTXUI only for `ftxui::string_width` (terminal cell widths of text), so they live in the `meradb_workbench`
library together with the view.

---

## Task index (details below; batches at the end)

Batches and where they are in this file:

| Batch | Tasks | Lines |
|---|---|---|
| A | 1-5 | 216-1587 |
| B | 6-8 | 1588-2319 |
| C | 9-12 | 2320-2976 |
| D | 13-14 | 2977-3279 |

| # | Title | Batch | Lines |
|---|---|---|---|
| 1 | FTXUI dependency, `MERADB_WORKBENCH` option, library skeleton, key probe | A | 218-485 |
| 2 | `highlight`: the lenient scanner, accessors and the Python golden | A | 486-877 |
| 3 | `wb_text`: styles, cells, CSV, echo lines, clipping, wrapping, history, scroll state | A | 878-1221 |
| 4 | `TextBuffer`: the editor model | A | 1222-1460 |
| 5 | `TreeModel`: the schema tree | A | 1461-1587 |
| 6 | `Worker`, `Session` core: running text, results, header, log | B | 1590-2017 |
| 7 | `Session` actions: explain, history, export, connect, quit, shutdown | B | 2018-2153 |
| 8 | Help: Markdown subset, embedded `LANGUAGE.md`, key table | B | 2154-2319 |
| 9 | FTXUI panels: tree, results, log, editor | C | 2335-2528 |
| 10 | The window: layout, header, footer, key routing, focus | C | 2529-2625 |
| 11 | Modals: help and connect dialogs, busy indicator, mouse | C | 2626-2739 |
| 12 | Terminal-mode guard, `runWorkbench`, CLI wiring | C | 2740-2976 |
| 13 | End-to-end: headless UI test and the Python Pilot comparison | D | 2979-3210 |
| 14 | Docs, manual terminal checklist, completion checklist | D | 3211-3279 |

---

## Command conventions used in every task

- Configure once (MinGW on Windows, from the repo root, PATH exported first):
  `cmake -S cpp -B cpp/build -G "MinGW Makefiles"` (Linux/macOS: omit `-G`). Re-run it whenever a task edits
  a `CMakeLists.txt`. The first configure after Task 1 downloads FTXUI. Without network:
  `-DMERADB_WORKBENCH=OFF`.
- Build: `cmake --build cpp/build`.
- Test everything: `ctest --test-dir cpp/build --output-on-failure`. One area:
  `ctest --test-dir cpp/build --output-on-failure -R "<name>"`. Catch2 test-case names start with the area
  word on purpose: `highlight`, `wbtext`, `wbeditor`, `wbtree`, `wbworker`, `wbsession`, `wbhelp`, `wbui`,
  `wbe2e`.
- Tests of the model and the view live in the executable `meradb_wb_tests` (only built when
  `MERADB_WORKBENCH` is ON); `highlight` tests live in the normal `meradb_tests`.
- "Expected: FAIL" steps mean the build breaks (missing header) or the test fails; either counts.
- Python oracle: run from the repo root with `PYTHONIOENCODING=utf-8`; Pilot snippets use
  `app.run_test(size=(200, 50))` and a temp `MERADB_DATA`.
- Golden files are regenerated with `python cpp/tests/gen_highlight_golden.py` (Task 2); never edit generated
  files by hand.

# BATCH A — foundations: the dependency, the highlighter, and the pure model pieces

### Task 1: FTXUI dependency, `MERADB_WORKBENCH` option, library skeleton, key probe

**Files:**
- Modify: `cpp/CMakeLists.txt`
- Create: `cpp/include/meradb/workbench.h`
- Create: `cpp/src/workbench.cpp` (a placeholder `runWorkbench` that Task 12 replaces)
- Create: `cpp/tests/test_wb_sanity.cpp`
- Create: `cpp/tests/tools/wb_keyprobe.cpp`
- Modify: `cpp/tests/CMakeLists.txt` (the `meradb_wb_tests` executable and the probe, only `if(TARGET meradb_workbench)`)

**Interfaces:**
- Produces: CMake option `MERADB_WORKBENCH`; target `meradb_workbench` (static library linking `meradb_core` and
  `ftxui::component ftxui::dom ftxui::screen`); the compile definition `MERADB_HAVE_WORKBENCH=1` on `meradb_cli`;
  the test executable `meradb_wb_tests` (grows in later tasks); the developer tool `wb_keyprobe`.
- `workbench.h` declares `namespace meradb::wb { int runWorkbench(std::unique_ptr<Backend> backend, const CliArgs& args); }`.

- [ ] **Step 1: Edit `cpp/CMakeLists.txt`.** Move `include(FetchContent)` and the `CMP0135` policy block *above* the
  `if(MSVC) add_compile_options(...)` block, and put the FTXUI block between them, so FTXUI is compiled without
  our `-Wall -Wextra` / `/W4` flags. The top of the file becomes:

```cmake
# cpp/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(meradb_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)  # extracted files get the time of extraction (silences FetchContent warning)
endif()

# The full-screen workbench needs FTXUI, downloaded on the first configure. Without network access configure with
# -DMERADB_WORKBENCH=OFF (the rest builds as before and `workbench` prints a note), or point
# -DFETCHCONTENT_SOURCE_DIR_FTXUI=<unpacked v5.0.0 tarball> at a copy you already have.
option(MERADB_WORKBENCH "Build the full-screen workbench (fetches FTXUI)" ON)
if(MERADB_WORKBENCH)
  # Declared before our own warning flags below: add_compile_options only reaches targets created after it.
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    ftxui
    URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v5.0.0.tar.gz
  )
  FetchContent_MakeAvailable(ftxui)
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8)   # sources contain UTF-8 (Devanagari, dashes) in comments
else()
  add_compile_options(-Wall -Wextra)
endif()

# (the json / catch2 FetchContent blocks, `list(APPEND CMAKE_MODULE_PATH ...)`, `include(Catch)` and the
#  meradb_core library are unchanged)
```

  After the `meradb_core` definition and before `add_executable(meradb_cli ...)`, add:

```cmake
if(MERADB_WORKBENCH)
  add_library(meradb_workbench STATIC
    src/workbench.cpp
    # workbench source files are appended here by later tasks
  )
  target_link_libraries(meradb_workbench PUBLIC meradb_core ftxui::component ftxui::dom ftxui::screen)
  # FTXUI's headers are third-party: keep our warning flags quiet about them.
  target_include_directories(meradb_workbench SYSTEM PUBLIC ${ftxui_SOURCE_DIR}/include)
endif()
```

  and replace the `meradb_cli` lines with:

```cmake
add_executable(meradb_cli src/main.cpp)
target_link_libraries(meradb_cli PRIVATE meradb_core)
if(MERADB_WORKBENCH)
  target_link_libraries(meradb_cli PRIVATE meradb_workbench)
  target_compile_definitions(meradb_cli PRIVATE MERADB_HAVE_WORKBENCH=1)
endif()
```

- [ ] **Step 2: Write `workbench.h` and the placeholder `workbench.cpp`.**

```cpp
// cpp/include/meradb/workbench.h
//
// The full-screen workbench (mirrors meradb/tui.py's run_workbench). Only built when the CMake option
// MERADB_WORKBENCH is ON; cli.cpp reaches it through a hook (cli.h: setWorkbenchRunner), never directly.
#pragma once
#include "meradb/backend.h"
#include "meradb/cli.h"
#include <memory>

namespace meradb::wb {

// Takes ownership of `backend`, runs the UI until the user quits, closes the backend (an unfinished
// transaction is rolled back) and returns the process exit code (0).
int runWorkbench(std::unique_ptr<Backend> backend, const CliArgs& args);

}  // namespace meradb::wb
```

```cpp
// cpp/src/workbench.cpp (placeholder; Task 12 replaces the body)
#include "meradb/workbench.h"

namespace meradb::wb {

int runWorkbench(std::unique_ptr<Backend> backend, const CliArgs&) {
    if (backend) backend->close();
    return 0;
}

}  // namespace meradb::wb
```

- [ ] **Step 3: Write the failing sanity test** (proves the dependency links and that headless rendering works):

```cpp
// cpp/tests/test_wb_sanity.cpp
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

using namespace ftxui;

TEST_CASE("wbsanity FTXUI renders into an in-memory screen", "[wbsanity]") {
    auto screen = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(3));
    Element row = hbox({text("ab"), text(" "), text("\xE6\x97\xA5\xE6\x9C\xAC") | bold});  // "ab 日本"
    Render(screen, row);
    CHECK(screen.PixelAt(0, 0).character == "a");
    CHECK(screen.PixelAt(3, 0).character == "\xE6\x97\xA5");
    CHECK(screen.PixelAt(3, 0).bold);
    CHECK_FALSE(screen.PixelAt(0, 0).bold);
}

TEST_CASE("wbsanity string_width counts terminal cells", "[wbsanity]") {
    CHECK(string_width("a") == 1);
    CHECK(string_width("\xC3\xA9") == 1);                 // é
    CHECK(string_width("\xE6\x97\xA5\xE6\x9C\xAC") == 4);  // 日本: two cells each
    CHECK(string_width("") == 0);
}

TEST_CASE("wbsanity events compare by their raw text", "[wbsanity]") {
    CHECK(Event::Special(std::string(1, '\x13')).input() == std::string(1, '\x13'));  // Ctrl+S
    CHECK(Event::F5 != Event::F6);
    CHECK(Event::ArrowUpCtrl != Event::ArrowUp);
    CHECK(Event::Character("x") == Event::Character('x'));
}
```

- [ ] **Step 4: Edit `cpp/tests/CMakeLists.txt`.** After the `catch_discover_tests(meradb_tests)` line add:

```cmake
if(TARGET meradb_workbench)
  add_executable(meradb_wb_tests
    test_wb_sanity.cpp
    # workbench test files are appended here by later tasks
  )
  target_link_libraries(meradb_wb_tests PRIVATE meradb_workbench meradb_core Catch2::Catch2WithMain)
  catch_discover_tests(meradb_wb_tests)

  # A developer tool, not a test: shows the raw bytes your terminal sends for each key (see Task 12 / 14).
  add_executable(wb_keyprobe tools/wb_keyprobe.cpp)
  target_link_libraries(wb_keyprobe PRIVATE meradb_workbench meradb_core)
endif()
```

- [ ] **Step 5: Write the key probe.**

```cpp
// cpp/tests/tools/wb_keyprobe.cpp -- run it in a real terminal: shows what FTXUI delivers for each key.
// Press the keys of the workbench (F1 F5 F6, Ctrl+Up/Down, Shift+arrows, Ctrl+S/R/O/L/Q/P/N/A/C, Esc, Tab,
// Shift+Tab, PageUp); press 'x' to quit.
#include "meradb/sys_compat.h"
#include <cstdio>
#include <deque>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

namespace {
std::string hex(const std::string& s) {
    std::string out;
    char buf[8];
    for (unsigned char c : s) {
        std::snprintf(buf, sizeof buf, "%02X ", c);
        out += buf;
    }
    return out;
}
std::string nameOf(const Event& e) {
    struct Named { const char* name; Event event; };
    const Named known[] = {{"ArrowUpCtrl", Event::ArrowUpCtrl}, {"ArrowDownCtrl", Event::ArrowDownCtrl},
                           {"ArrowLeftCtrl", Event::ArrowLeftCtrl}, {"ArrowRightCtrl", Event::ArrowRightCtrl},
                           {"F1", Event::F1}, {"F5", Event::F5}, {"F6", Event::F6}, {"Tab", Event::Tab},
                           {"TabReverse", Event::TabReverse}, {"Escape", Event::Escape}, {"Return", Event::Return},
                           {"Backspace", Event::Backspace}, {"Delete", Event::Delete}, {"PageUp", Event::PageUp},
                           {"Home", Event::Home}, {"End", Event::End}};
    for (const auto& k : known)
        if (e == k.event) return std::string(" = ") + k.name;
    return "";
}
}  // namespace

int main() {
    meradb::sys::Utf8Console console;
    auto screen = ScreenInteractive::Fullscreen();
    std::deque<std::string> seen;
    auto view = Renderer([&] {
        Elements rows;
        rows.push_back(text("Key probe: press keys, newest first; 'x' quits.") | bold);
        for (const auto& line : seen) rows.push_back(text(line));
        return vbox(std::move(rows)) | border;
    });
    auto app = CatchEvent(view, [&](Event e) {
        if (e == Event::Character('x')) {
            screen.ExitLoopClosure()();
            return true;
        }
        std::string line = "bytes: " + hex(e.input()) + (e.is_character() ? "[character] " : "") +
                           (e.is_mouse() ? "[mouse] " : "") + nameOf(e);
        seen.push_front(line);
        if (seen.size() > 20) seen.pop_back();
        return true;
    });
    screen.Loop(app);
    return 0;
}
```

- [ ] **Step 6: Configure, build, run.** `cmake -S cpp -B cpp/build -G "MinGW Makefiles"` (downloads FTXUI),
  `cmake --build cpp/build`, `ctest --test-dir cpp/build -R wbsanity --output-on-failure`. Expected: PASS, and no
  warning from FTXUI sources (they are built before the flags are set). If MinGW g++ rejects an FTXUI header
  (a missing `<cstdint>`-class error), fix it by adding `-include cstdint` for the FTXUI targets only
  (`target_compile_options(screen PRIVATE -include cstdint)` right after `FetchContent_MakeAvailable`); do not
  change versions without telling the plan owner. Also confirm `cmake -S cpp -B cpp/build-off -DMERADB_WORKBENCH=OFF`
  configures and builds without fetching FTXUI (delete the folder afterwards).

- [ ] **Step 7: Run the key probe by hand** (`cpp/build/tests/wb_keyprobe` or wherever the generator put it) in
  Windows Terminal and, if available, a classic console and a Linux/macOS terminal. Write down for the commit
  message and for Task 14's checklist: (a) the bytes of Ctrl+Up, Ctrl+Down, Shift+Up/Down/Left/Right/Home/End,
  F1, F5, F6; (b) whether Ctrl+S / Ctrl+Q / Ctrl+O / Ctrl+C reach the program as bytes `13` / `11` / `0F` /
  `03` or are swallowed (POSIX: Ctrl+S freezes the terminal until Ctrl+Q; Ctrl+C exits the probe, which is
  FTXUI's SIGINT handling); (c) what a pasted emoji and a Devanagari letter look like. Tasks 9-12 assume:
  Ctrl+Up/Down arrive as `ArrowUpCtrl` / `ArrowDownCtrl`, Shift+arrows as `1B 5B 31 3B 32 41..44`, Ctrl+letters as
  the control bytes. If a terminal differs, the aliases of R12 and the selection fallback (Ctrl+A) are the
  safety net; record the difference, do not guess.

- [ ] **Step 8: Commit.**

```
git add cpp/CMakeLists.txt cpp/include/meradb/workbench.h cpp/src/workbench.cpp cpp/tests/CMakeLists.txt cpp/tests/test_wb_sanity.cpp cpp/tests/tools/wb_keyprobe.cpp
git commit -m "Add the FTXUI dependency behind MERADB_WORKBENCH, a library skeleton and a key probe"
```

---

### Task 2: `highlight`: the lenient scanner, accessors and the Python golden

**Files:**
- Create: `cpp/include/meradb/highlight.h`, `cpp/src/highlight.cpp`
- Modify: `cpp/include/meradb/tokenizer.h`, `cpp/src/tokenizer.cpp` (add `bool isKeyword(const std::string& upperWord)`)
- Modify: `cpp/include/meradb/aggregates.h`, `cpp/src/aggregates.cpp` (add `bool isAggregateName(const std::string& upperWord)`)
- Create: `cpp/tests/gen_highlight_golden.py` (writes `cpp/tests/golden_highlight.h`), generated `cpp/tests/golden_highlight.h`
- Create: `cpp/tests/test_highlight.cpp`
- Modify: `cpp/CMakeLists.txt` (`src/highlight.cpp` into `meradb_core`), `cpp/tests/CMakeLists.txt` (`test_highlight.cpp` into `meradb_tests`)

**Interfaces:**
- `namespace meradb { bool isKeyword(const std::string& upper); bool isAggregateName(const std::string& upper); }`
  — `isKeyword` is `keywordSet().count(upper) != 0` (the set tokenizer.cpp already has); `isAggregateName` is true for
  `GINO COUNT KUL SUM AUSAT AVG NYUNTAM MIN ADHIKTAM MAX` (Python's `aggregates.ALIASES` keys; add a comment
  naming the source).
- `namespace meradb::highlight`:

```cpp
// cpp/include/meradb/highlight.h
//
// Syntax highlighting for MeraDB queries (mirrors meradb/highlight.py). LENIENT on purpose: the strict tokenizer
// throws on `'unterminated`, which is exactly what the text looks like while you type. Spans are BYTE offsets
// into the line (Python's are characters; the golden generator converts), `kindName` gives the same names as
// Python's `kind` strings.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace meradb::highlight {

enum class Kind { Keyword, String, Number, Comment, Operator, Type, Boolean, ConstantBuiltin, Function, Bracket };

struct Span {
    std::size_t start;
    std::size_t end;  // exclusive
    Kind kind;
};

// "keyword" "string" "number" "comment" "operator" "type" "boolean" "constant.builtin" "function" "punctuation.bracket"
const char* kindName(Kind kind);

// Highlight spans for ONE line (no '\n'), in order, never overlapping.
std::vector<Span> spans(const std::string& line);

// highlight.py's RICH_STYLES: rgb is 0xRRGGBB or -1 for "no colour".
struct RichStyle {
    int rgb;
    bool bold;
    bool italic;
};
RichStyle richStyle(Kind kind);

}  // namespace meradb::highlight
```

- [ ] **Step 1: Write the generator** `cpp/tests/gen_highlight_golden.py`. It imports the Python highlighter, builds
  a corpus (every line of `examples/demo.mdb` and `examples/rdbms_lab_coverage.mdb`, the edge lines below, and for
  every word in `KEYWORDS ∪ TYPE_ALIASES ∪ ALIASES` the three lines `word`, `word(`, `word (` plus the
  lower-case form of each), computes the spans with **byte** offsets and writes the header. Escape every
  non-printable byte, `"`, `\` and `?` (trigraph-proof) as a 3-digit octal escape.

```python
#!/usr/bin/env python3
"""Regenerates cpp/tests/golden_highlight.h: what meradb/highlight.py returns for many lines (byte offsets)."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from meradb.aggregates import ALIASES  # noqa: E402
from meradb.datatypes import TYPE_ALIASES  # noqa: E402
from meradb.highlight import spans  # noqa: E402
from meradb.tokenizer import KEYWORDS  # noqa: E402

EDGE = [
    "", "   ", "-- only a comment", "x -- c", "'a'", "'a''b'", "'a'''", "'a''", "'", "''",
    "a<=b>=c!=d<>e", "a!b", "1.5.5", "1..2", "x1 1x _1 a_b", "12 3.5 1abc 7.x", "(()", "DIKHAO\t*\tSE\tt",
    "dikhao Dikhao DIKHAO", "SACH sach JhOoTh KHALI khali", "gino (*)", "gino   (", "count(*) COUNT (x)",
    "max(a) MAX b", "x = 'it''s' -- note", "'é' é -- ü", "12é 3", "é" "12", "1.5é",
    " DIKHAO", "DIKHAO (", "x;y,z.w", "a % b / c + d - e * f",
]


def corpus():
    seen = []

    def add(line):
        if line not in seen:
            seen.append(line)

    for name in ("demo.mdb", "rdbms_lab_coverage.mdb"):
        text = (ROOT / "examples" / name).read_text(encoding="utf-8-sig")
        for line in text.split("\n"):
            add(line.rstrip("\r"))
    for line in EDGE:
        add(line)
    for word in sorted(set(KEYWORDS) | set(TYPE_ALIASES) | set(ALIASES)):
        for w in (word, word.lower()):
            add(w)
            add(w + "(")
            add(w + " (")
    return seen


def cpp_string(text):
    out = []
    for b in text.encode("utf-8"):
        if b in (34, 92, 63) or not 32 <= b < 127:
            out.append("\\%03o" % b)
        else:
            out.append(chr(b))
    return '"' + "".join(out) + '"'


def byte_spans(line):
    return ";".join(
        "%d,%d,%s" % (len(line[:s].encode("utf-8")), len(line[:e].encode("utf-8")), kind) for s, e, kind in spans(line)
    )


def main():
    lines = ["// cpp/tests/golden_highlight.h -- GENERATED by gen_highlight_golden.py from meradb/highlight.py; do not edit.",
             "#pragma once", "", "struct HighlightCase {", "    const char* line;", "    const char* spans;  // \"start,end,kind;...\" (byte offsets)", "};", "",
             "inline const HighlightCase kHighlightCases[] = {"]
    for line in corpus():
        lines.append("    {%s, %s}," % (cpp_string(line), cpp_string(byte_spans(line))))
    lines.append("};")
    (ROOT / "cpp" / "tests" / "golden_highlight.h").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("wrote %d cases" % len(corpus()))


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Write the failing test** `cpp/tests/test_highlight.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "golden_highlight.h"
#include "meradb/aggregates.h"
#include "meradb/highlight.h"
#include "meradb/tokenizer.h"

using namespace meradb;
using namespace meradb::highlight;

namespace {
std::string render(const std::string& line) {
    std::string out;
    for (const Span& s : spans(line)) {
        if (!out.empty()) out += ";";
        out += std::to_string(s.start) + "," + std::to_string(s.end) + "," + kindName(s.kind);
    }
    return out;
}
}  // namespace

TEST_CASE("highlight finds keywords, operators and skips plain names", "[highlight]") {
    CHECK(render("DIKHAO * SE t;") == "0,6,keyword;7,8,operator;9,11,keyword");
    CHECK(render("SACH jhooth khali") == "0,4,boolean;5,11,boolean;12,17,constant.builtin");
    CHECK(render("") == "");
}

TEST_CASE("highlight strings, comments and numbers", "[highlight]") {
    CHECK(render("x = 'it''s' -- note") == "2,3,operator;4,11,string;12,19,comment");
    CHECK(render("'abc") == "0,4,string");  // the closing quote is optional while typing
    CHECK(render("12 3.5 1abc 7.x") == "0,2,number;3,6,number;12,13,number");
}

TEST_CASE("highlight accessors share the tokenizer's word lists", "[highlight]") {
    CHECK(isKeyword("DIKHAO"));
    CHECK_FALSE(isKeyword("STUDENTS"));
    CHECK(isAggregateName("COUNT"));
    CHECK(isAggregateName("ADHIKTAM"));
    CHECK_FALSE(isAggregateName("DIKHAO"));
}

TEST_CASE("highlight matches Python's highlight.py on every golden line", "[highlight]") {
    for (const HighlightCase& c : kHighlightCases) {
        INFO("line: " << c.line);
        CHECK(render(c.line) == c.spans);
    }
}

TEST_CASE("highlight rich styles are the Dracula table of highlight.py", "[highlight]") {
    CHECK(richStyle(Kind::Keyword).rgb == 0xff79c6);
    CHECK(richStyle(Kind::Keyword).bold);
    CHECK(richStyle(Kind::Comment).italic);
    CHECK(richStyle(Kind::Bracket).rgb == -1);
    CHECK(richStyle(Kind::ConstantBuiltin).italic);
    CHECK(richStyle(Kind::Function).rgb == 0x50fa7b);
}
```

- [ ] **Step 3: Run `python cpp/tests/gen_highlight_golden.py`, add the sources to the two `CMakeLists.txt`, build.**
  Expected: FAIL (no `highlight.h`).

- [ ] **Step 4: Implement.** The scanner is a hand translation of the regex. Alternation order at each position
  is comment, string, number, word, operator, bracket; a position where nothing matches advances by one byte.
  Rules, each one a line of the regex:

  - **comment** `--.*`: from `--` to the next `'\n'` (or the end).
  - **string** `'(?:[^']|'')*'?`: from the quote; a doubled quote stays inside; the closing quote is optional.
  - **number** `\b\d+(?:\.\d+)?\b` (ASCII digits): requires that the previous byte is not a word byte; take the digit
    run; if `.` + digit follows, try the longer match first and accept it only when the byte after it is not a
    word byte; otherwise fall back to the integer part if the byte after *that* is not a word byte (a `.` is not);
    else no number here (advance one byte; `1abc` yields no number, and `abc` is then read as a word).
  - **word** `[A-Za-z_][A-Za-z0-9_]*`: classify the upper-cased word: `SACH`/`JHOOTH` boolean; `KHALI`
    constant.builtin; `isKeyword` keyword; `normalizeType(upper)` has a value: type; `isAggregateName(upper)` *and*
    `pytext::lstrip(rest of line)` starts with `(`: function; anything else: no span.
  - **operator** `<=|>=|!=|<>|[=<>+\-*/%]` (two-byte forms first), **bracket** `[()]` (kind `punctuation.bracket`).
  - "Word byte" = ASCII letter, digit, `_`, or any byte >= 0x80 (ruling R11).

```cpp
// cpp/src/highlight.cpp
#include "meradb/highlight.h"
#include "meradb/aggregates.h"
#include "meradb/datatypes.h"
#include "meradb/pytext.h"
#include "meradb/tokenizer.h"

namespace meradb::highlight {

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
bool isAsciiAlpha(unsigned char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isAsciiWord(unsigned char c) { return isAsciiAlpha(c) || isDigit(c); }
bool isWordByte(unsigned char c) { return isAsciiWord(c) || c >= 0x80; }

bool boundaryAfter(const std::string& s, std::size_t end) {
    return end >= s.size() || !isWordByte(static_cast<unsigned char>(s[end]));
}

std::size_t numberEnd(const std::string& s, std::size_t i) {
    if (i > 0 && isWordByte(static_cast<unsigned char>(s[i - 1]))) return kNone;  // no \b before the first digit
    std::size_t j = i;
    while (j < s.size() && isDigit(static_cast<unsigned char>(s[j]))) ++j;
    if (j + 1 < s.size() && s[j] == '.' && isDigit(static_cast<unsigned char>(s[j + 1]))) {
        std::size_t k = j + 1;
        while (k < s.size() && isDigit(static_cast<unsigned char>(s[k]))) ++k;
        if (boundaryAfter(s, k)) return k;
    }
    return boundaryAfter(s, j) ? j : kNone;
}

std::string upperAscii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

bool classifyWord(const std::string& line, std::size_t start, std::size_t end, Kind& kind) {
    const std::string upper = upperAscii(line.substr(start, end - start));
    if (upper == "SACH" || upper == "JHOOTH") kind = Kind::Boolean;
    else if (upper == "KHALI") kind = Kind::ConstantBuiltin;
    else if (isKeyword(upper)) kind = Kind::Keyword;
    else if (normalizeType(upper).has_value()) kind = Kind::Type;
    else if (isAggregateName(upper)) {
        const std::string rest = pytext::lstrip(line.substr(end));
        if (rest.empty() || rest[0] != '(') return false;
        kind = Kind::Function;
    } else return false;
    return true;
}

}  // namespace

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Keyword: return "keyword";
        case Kind::String: return "string";
        case Kind::Number: return "number";
        case Kind::Comment: return "comment";
        case Kind::Operator: return "operator";
        case Kind::Type: return "type";
        case Kind::Boolean: return "boolean";
        case Kind::ConstantBuiltin: return "constant.builtin";
        case Kind::Function: return "function";
        case Kind::Bracket: return "punctuation.bracket";
    }
    return "";
}

std::vector<Span> spans(const std::string& s) {
    std::vector<Span> out;
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '-' && i + 1 < n && s[i + 1] == '-') {
            std::size_t e = s.find('\n', i);
            if (e == std::string::npos) e = n;
            out.push_back({i, e, Kind::Comment});
            i = e;
            continue;
        }
        if (c == '\'') {
            std::size_t j = i + 1;
            while (j < n) {
                if (s[j] == '\'') {
                    if (j + 1 < n && s[j + 1] == '\'') { j += 2; continue; }
                    ++j;  // the closing quote
                    break;
                }
                ++j;
            }
            out.push_back({i, j, Kind::String});
            i = j;
            continue;
        }
        if (isDigit(c)) {
            const std::size_t e = numberEnd(s, i);
            if (e != kNone) {
                out.push_back({i, e, Kind::Number});
                i = e;
            } else {
                ++i;
            }
            continue;
        }
        if (isAsciiAlpha(c)) {
            std::size_t j = i + 1;
            while (j < n && isAsciiWord(static_cast<unsigned char>(s[j]))) ++j;
            Kind kind;
            if (classifyWord(s, i, j, kind)) out.push_back({i, j, kind});
            i = j;
            continue;
        }
        if (i + 1 < n && ((c == '<' && (s[i + 1] == '=' || s[i + 1] == '>')) || ((c == '>' || c == '!') && s[i + 1] == '='))) {
            out.push_back({i, i + 2, Kind::Operator});
            i += 2;
            continue;
        }
        if (c == '=' || c == '<' || c == '>' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
            out.push_back({i, i + 1, Kind::Operator});
            ++i;
            continue;
        }
        if (c == '(' || c == ')') {
            out.push_back({i, i + 1, Kind::Bracket});
            ++i;
            continue;
        }
        ++i;
    }
    return out;
}

RichStyle richStyle(Kind kind) {
    switch (kind) {
        case Kind::Keyword: return {0xff79c6, true, false};
        case Kind::String: return {0xf1fa8c, false, false};
        case Kind::Number: return {0xbd93f9, false, false};
        case Kind::Comment: return {0x6272a4, false, true};
        case Kind::Operator: return {0xff79c6, false, false};
        case Kind::Type: return {0x8be9fd, false, false};
        case Kind::Boolean: return {0xbd93f9, false, false};
        case Kind::ConstantBuiltin: return {0xbd93f9, false, true};
        case Kind::Function: return {0x50fa7b, false, false};
        case Kind::Bracket: return {-1, false, false};
    }
    return {-1, false, false};
}

}  // namespace meradb::highlight
```

  Add the two accessors (declare in the headers with a one-line comment each) and add `src/highlight.cpp` to
  `meradb_core`.

- [ ] **Step 5: Run `ctest --test-dir cpp/build -R highlight --output-on-failure`.** Expected: PASS. A golden line
  that fails because of the Unicode `\b` / `\d` approximation (R11) is **removed from the generator's corpus**
  (add a `DROP` set with a comment naming the line), not "fixed" in the scanner; list each one in Task 14's
  divergence text. Any other difference is a scanner bug.

- [ ] **Step 6: Mutation check.** Change one expected kind in a hand-written test (for example `keyword` to `type`)
  and confirm the test fails; revert.

- [ ] **Step 7: Commit.**

```
git add cpp/include/meradb/highlight.h cpp/src/highlight.cpp cpp/include/meradb/tokenizer.h cpp/src/tokenizer.cpp cpp/include/meradb/aggregates.h cpp/src/aggregates.cpp cpp/tests/gen_highlight_golden.py cpp/tests/golden_highlight.h cpp/tests/test_highlight.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the query highlighter and check it against highlight.py"
```

---

### Task 3: `wb_text`: styles, cells, CSV, echo lines, clipping, wrapping, history, scroll state

**Files:**
- Create: `cpp/include/meradb/wb_text.h`, `cpp/src/wb_text.cpp`, `cpp/tests/test_wb_text.cpp`
- Modify: `cpp/CMakeLists.txt` (append `src/wb_text.cpp` to `meradb_workbench`), `cpp/tests/CMakeLists.txt` (append `test_wb_text.cpp` to `meradb_wb_tests`)

**Interfaces** (everything in `namespace meradb::wb`; all pure, no terminal):

```cpp
// cpp/include/meradb/wb_text.h
#pragma once
#include "meradb/engine.h"
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace meradb::wb {

// ---- styles ----
struct Style {
    int fg = -1;  // 0xRRGGBB, -1 = the terminal's default
    int bg = -1;
    bool bold = false, dim = false, italic = false, underline = false, inverse = false;
    bool operator==(const Style& o) const {
        return fg == o.fg && bg == o.bg && bold == o.bold && dim == o.dim && italic == o.italic &&
               underline == o.underline && inverse == o.inverse;
    }
    bool operator!=(const Style& o) const { return !(*this == o); }
};
namespace palette {  // tui.py / highlight.py (Dracula)
constexpr int kPink = 0xff79c6, kString = 0xf1fa8c, kPurple = 0xbd93f9, kComment = 0x6272a4, kCyan = 0x8be9fd,
              kGreen = 0x50fa7b, kOrange = 0xffb86c, kRed = 0xff5555, kYellow = 0xf1fa8c, kText = 0xf8f8f2,
              kBackground = 0x282a36, kCurrentLine = 0x44475a, kStripe = 0x2f3142;
}
Style fgStyle(int rgb, bool bold = false, bool dim = false, bool italic = false);

struct Segment { std::string text; Style style; };
using Line = std::vector<Segment>;
std::string plainText(const Line& line);
void appendSegment(Line& line, const std::string& text, const Style& style);  // merges equal neighbours, skips ""

// ---- the log ----
enum class LogKind { Plain, Bold, Dim, Error, Message, Connected, Warn, Echo };
const char* logKindName(LogKind kind);  // plain bold dim error message connected warn echo
Style styleForLogKind(LogKind kind);
struct LogEntry { LogKind kind = LogKind::Plain; std::vector<Line> lines; };
class LogBuffer {
public:
    static constexpr std::size_t kMaxLines = 20000;
    void add(LogEntry entry);                                  // drops the oldest entries past kMaxLines lines
    void addText(LogKind kind, const std::string& text);       // text split at '\n', all lines in styleForLogKind
    void clear();
    const std::deque<LogEntry>& entries() const { return entries_; }
    std::size_t lineCount() const { return lines_; }
    std::size_t revision() const { return revision_; }      // bumps on every add / clear (view caches key on it)
    std::size_t generation() const { return generation_; }  // bumps when old entries disappear (clear, trimming)
private:
    std::deque<LogEntry> entries_;
    std::size_t lines_ = 0, revision_ = 0, generation_ = 0;
};

// Python's str.splitlines(): \n \r\n \r \v \f \x1c \x1d \x1e U+0085 U+2028 U+2029; no empty last line; "" -> {}.
std::vector<std::string> splitLines(const std::string& text);
Line highlightLine(const std::string& line);               // highlight::spans -> styled segments (RICH_STYLES)
// tui.py's `Text(f"{db}> ", dim) + highlighted(text)`: lines of text.strip().splitlines(), prefix on the first.
LogEntry echoEntry(const std::string& db, const std::string& text);

// ---- the results table ----
enum class CellKind { Null, True, False, Number, Plain };
const char* cellKindName(CellKind kind);  // null true false number plain
struct Cell { std::string text; CellKind kind = CellKind::Plain; };
Cell makeCell(const Value& value);        // tui.py cell(): KHALI / SACH / JHOOTH / number / text (R16 for newlines)
Style cellStyle(CellKind kind);
inline bool cellRightAligned(CellKind kind) { return kind == CellKind::Number; }
struct ResultTable { std::vector<std::string> columns; std::vector<std::vector<Cell>> rows; };
ResultTable makeTable(const Result& result);
std::vector<int> columnWidths(const ResultTable& table);        // display width of the widest of header and cells
std::string resultsTitle(const ResultTable* table);             // "Results" or "Results -- N row(s)"

// ---- cell geometry (terminal cells, wide characters count 2) ----
struct Glyph { std::string text; int width; };                  // one character with its combining marks
std::vector<Glyph> glyphs(const std::string& utf8);
Line clipLine(const Line& line, int skipCells, int takeCells);  // the part in [skip, skip+take); a cut wide glyph becomes ' '
std::vector<Line> wrapLine(const Line& line, int width);        // character wrap, at least one (maybe empty) line

// ---- CSV (Python csv.writer defaults) ----
std::string csvRow(const std::vector<std::string>& fields);     // quoted as needed, ends "\r\n"
std::string csvDocument(const Result& result);                  // header row, then rows (KHALI -> "", else formatValue)
std::string exportFileStamp();                                  // "20260929-140307" (local time)
struct ExportOutcome { bool ok = false; std::string path; std::string error; };
// Writes <baseDir>/exports/meradb-<stamp>.csv ("" = current directory), UTF-8, bytes exactly as csvDocument.
ExportOutcome exportCsv(const Result& result, const std::string& baseDir, const std::string& stamp);

// ---- history and scrolling ----
class History {
public:
    void add(const std::string& text);                  // appended unless equal to the last entry; position = size
    std::optional<std::string> step(int delta);         // nullopt when empty; pos = clamp(pos+delta, 0, size); "" at size
    std::size_t size() const { return items_.size(); }
    const std::vector<std::string>& items() const { return items_; }
private:
    std::vector<std::string> items_;
    std::size_t pos_ = 0;
};

class ScrollState {  // a window of `height` rows over `count` rows, plus an optional cursor row
public:
    void setCount(int n);
    void setHeight(int h);
    int count() const { return count_; }
    int height() const { return height_; }
    int top() const { return top_; }
    int cursor() const { return cursor_; }
    void setCursor(int row);            // clamped; scrolls just enough to keep it visible
    void moveCursor(int delta);
    void pageCursor(int pages);         // pages * (height - 1) rows
    void scrollBy(int delta);           // moves the window only (the cursor may leave it)
    void ensureVisible(int row);
    void followEnd();                   // top = last full window
    bool atBottom() const;
private:
    int maxTop() const;
    void clamp();
    int count_ = 0, height_ = 1, top_ = 0, cursor_ = 0;
};

}  // namespace meradb::wb
```

- [ ] **Step 1: Write the failing tests** `cpp/tests/test_wb_text.cpp` (names start `wbtext`). Cover, with these exact
  expectations:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/wb_text.h"
#include "test_util.h"
#include <fstream>

using namespace meradb;
using namespace meradb::wb;

TEST_CASE("wbtext cells follow tui.py cell()", "[wbtext]") {
    CHECK(makeCell(Value()).text == "KHALI");
    CHECK(makeCell(Value()).kind == CellKind::Null);
    CHECK(makeCell(Value(true)).text == "SACH");
    CHECK(makeCell(Value(true)).kind == CellKind::True);
    CHECK(makeCell(Value(false)).kind == CellKind::False);
    CHECK(makeCell(Value(int64_t(42))).kind == CellKind::Number);
    CHECK(makeCell(Value(8.5)).text == "8.5");
    CHECK(makeCell(Value(std::string("hi"))).kind == CellKind::Plain);
    CHECK(makeCell(Value(std::string("a\nb\tc"))).text == "a\xE2\x86\xB5" "b c");  // R16
    CHECK(cellRightAligned(CellKind::Number));
    CHECK_FALSE(cellRightAligned(CellKind::Plain));
    CHECK(cellStyle(CellKind::True).fg == palette::kGreen);
    CHECK(cellStyle(CellKind::False).fg == palette::kRed);
    CHECK(cellStyle(CellKind::Null).dim);
    CHECK(cellStyle(CellKind::Null).italic);
    CHECK(cellStyle(CellKind::Number).fg == palette::kPurple);
}

TEST_CASE("wbtext table, widths and title", "[wbtext]") {
    Result r;
    r.columns = {"id", "naam"};
    r.rows.push_back({Value(int64_t(1)), Value(std::string("Asha"))});
    r.rows.push_back({Value(int64_t(22)), Value()});
    ResultTable t = makeTable(r);
    CHECK(t.rows.size() == 2);
    CHECK(columnWidths(t) == std::vector<int>({2, 5}));  // "22", "KHALI"
    CHECK(resultsTitle(&t) == "Results -- 2 row(s)");
    CHECK(resultsTitle(nullptr) == "Results");
}

TEST_CASE("wbtext splitLines matches str.splitlines", "[wbtext]") {
    CHECK(splitLines("").empty());
    CHECK(splitLines("a\n") == std::vector<std::string>({"a"}));
    CHECK(splitLines("a\n\nb") == std::vector<std::string>({"a", "", "b"}));
    CHECK(splitLines("a\r\nb\rc") == std::vector<std::string>({"a", "b", "c"}));
    CHECK(splitLines("a\x0b" "b\x0c" "c\x1c" "d") == std::vector<std::string>({"a", "b", "c", "d"}));
    CHECK(splitLines("a\xC2\x85" "b\xE2\x80\xA8" "c\xE2\x80\xA9" "d") == std::vector<std::string>({"a", "b", "c", "d"}));
    CHECK(splitLines("\n") == std::vector<std::string>({""}));
}

TEST_CASE("wbtext echo entry is a dim prompt plus the highlighted statement", "[wbtext]") {
    LogEntry e = echoEntry("main", "  DIKHAO * SE t;\nx  ");
    CHECK(e.kind == LogKind::Echo);
    REQUIRE(e.lines.size() == 2);
    CHECK(plainText(e.lines[0]) == "main> DIKHAO * SE t;");
    CHECK(e.lines[0][0].style.dim);
    CHECK(plainText(e.lines[1]) == "x");
    // the keyword is bold pink, the operator pink
    bool sawKeyword = false;
    for (const Segment& s : e.lines[0])
        if (s.text == "DIKHAO") sawKeyword = s.style.bold && s.style.fg == 0xff79c6;
    CHECK(sawKeyword);
}

TEST_CASE("wbtext log keeps at most kMaxLines lines and clears", "[wbtext]") {
    LogBuffer log;
    for (std::size_t i = 0; i < LogBuffer::kMaxLines + 5; ++i) log.addText(LogKind::Plain, "x");
    CHECK(log.lineCount() == LogBuffer::kMaxLines);
    log.addText(LogKind::Error, "a\nb");
    CHECK(log.entries().back().lines.size() == 2);
    CHECK(log.entries().back().lines[0][0].style.fg == palette::kRed);
    log.clear();
    CHECK(log.lineCount() == 0);
}

TEST_CASE("wbtext clip and wrap respect wide characters", "[wbtext]") {
    Line l;
    appendSegment(l, "ab\xE6\x97\xA5\xE6\x9C\xAC" "cd", Style{});  // ab日本cd : cells a b 日(2) 本(2) c d
    CHECK(plainText(clipLine(l, 0, 3)) == "ab ");                     // 日 does not fit in the last cell
    CHECK(plainText(clipLine(l, 3, 3)) == " \xE6\x9C\xAC");          // starts inside 日: blank, then 本
    CHECK(plainText(clipLine(l, 6, 10)) == "cd");
    CHECK(plainText(clipLine(l, 20, 5)) == "");
    auto w = wrapLine(l, 4);
    REQUIRE(w.size() == 3);
    CHECK(plainText(w[0]) == "ab\xE6\x97\xA5");
    CHECK(plainText(w[1]) == "\xE6\x9C\xAC" "cd");
    CHECK(wrapLine(Line{}, 5).size() == 1);
    Line a; appendSegment(a, "abcdef", fgStyle(1)); 
    auto w2 = wrapLine(a, 4);
    CHECK(plainText(w2[0]) == "abcd");
    CHECK(plainText(w2[1]) == "ef");
    CHECK(w2[1][0].style.fg == 1);
}

TEST_CASE("wbtext csv follows Python csv.writer", "[wbtext]") {
    CHECK(csvRow({"a", "b"}) == "a,b\r\n");
    CHECK(csvRow({"a,b", "c\"d", "e\nf"}) == "\"a,b\",\"c\"\"d\",\"e\nf\"\r\n");
    CHECK(csvRow({""}) == "\"\"\r\n");        // a lone empty field is quoted
    CHECK(csvRow({"", ""}) == ",\r\n");
    CHECK(csvRow({" x ", "\xC3\xA9"}) == " x ,\xC3\xA9\r\n");
    Result r;
    r.columns = {"id", "naam"};
    r.rows.push_back({Value(int64_t(1)), Value()});
    r.rows.push_back({Value(true), Value(std::string("a,b"))});
    CHECK(csvDocument(r) == "id,naam\r\n1,\r\nSACH,\"a,b\"\r\n");
    CHECK(exportFileStamp().size() == 15);
}

TEST_CASE("wbtext exportCsv writes exports/meradb-<stamp>.csv byte for byte", "[wbtext]") {
    meradb_test::TempDir dir;
    Result r;
    r.columns = {"x"};
    r.rows.push_back({Value(std::string("a"))});
    ExportOutcome out = exportCsv(r, dir.str(), "20260101-000000");
    REQUIRE(out.ok);
    CHECK(out.path.find("meradb-20260101-000000.csv") != std::string::npos);
    std::ifstream in(out.path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(bytes == "x\r\na\r\n");
}

TEST_CASE("wbtext exportCsv reports a failure instead of throwing", "[wbtext]") {
    meradb_test::TempDir dir;
    { std::ofstream blocker(dir.file("exports")); blocker << "a file where the folder should be"; }
    Result r;
    r.columns = {"x"};
    ExportOutcome out = exportCsv(r, dir.str(), "20260101-000000");
    CHECK_FALSE(out.ok);
    CHECK_FALSE(out.error.empty());
}

TEST_CASE("wbtext history follows tui.py", "[wbtext]") {
    History h;
    CHECK_FALSE(h.step(-1).has_value());
    h.add("a"); h.add("b"); h.add("b");  // consecutive duplicate ignored
    CHECK(h.size() == 2);
    CHECK(*h.step(-1) == "b");
    CHECK(*h.step(-1) == "a");
    CHECK(*h.step(-1) == "a");   // clamped at the oldest
    CHECK(*h.step(1) == "b");
    CHECK(*h.step(1) == "");     // past the newest: an empty editor
    CHECK(*h.step(1) == "");
    h.add("c");                  // position returns to the end
    CHECK(*h.step(-1) == "c");
}

TEST_CASE("wbtext scroll state keeps the cursor visible", "[wbtext]") {
    ScrollState s;
    s.setCount(100);
    s.setHeight(10);
    s.moveCursor(15);
    CHECK(s.cursor() == 15);
    CHECK(s.top() == 6);
    s.setCursor(0);
    CHECK(s.top() == 0);
    s.pageCursor(1);
    CHECK(s.cursor() == 9);
    s.scrollBy(-5);
    CHECK(s.top() == 0);
    s.followEnd();
    CHECK(s.top() == 90);
    CHECK(s.atBottom());
    s.setCount(5);   // shrinking clamps everything
    CHECK(s.top() == 0);
    CHECK(s.cursor() <= 4);
}
```

- [ ] **Step 2: Run to see it fail** (no header). Expected: FAIL.

- [ ] **Step 3: Implement `wb_text.cpp`.** Notes that matter:

  - `makeCell`: `std::holds_alternative<bool>` first (bool before integers: `Value::Storage` is a variant, so
    test alternatives, never convert), then `int64_t` / `double` -> `Number`, `monostate` -> `Null`, string and
    `Date` -> `Plain`. Text is `formatValue(v)`, then `\r` and `\n` become `↵` (`"\xE2\x86\xB5"`) and `\t`
    becomes a space. `cellStyle`: Null `dim+italic`; True `fgStyle(kGreen)`; False `fgStyle(kRed)`; Number
    `fgStyle(kPurple)`; Plain default. (tui.py's `green` / `red` are Rich colour names; R10 fixes the RGB.)
  - `makeTable`: `columns = result.columns`, one `Cell` per value.
  - `columnWidths`: `max(string_width(header), string_width(cell.text)...)` (include `<ftxui/screen/string.hpp>`).
  - `glyphs`: decode UTF-8 code points (lead byte -> length 1-4, clamped to the remaining bytes; a stray
    continuation byte is a one-byte glyph); `w = ftxui::string_width(cp)`; a code point of width 0 is appended to
    the previous glyph's text (or starts a width-0 glyph when first).
  - `clipLine`: walk glyphs with a running cell position `pos`; skip glyphs entirely before `skip`; a glyph that
    straddles `skip` contributes `' '` for each of its cells inside the window; stop when `take` cells are filled,
    and a glyph that does not fit entirely is replaced by `' '` for the remaining cells. Merge neighbours with
    `appendSegment`.
  - `wrapLine`: as in the test: a glyph that would overflow starts a new line unless the line is empty.
  - `csvRow`: a field needs quotes when it contains `,` `"` `\r` or `\n`; quotes are doubled; a single-field row whose
    field is empty is written `""`. `csvDocument` rows use `formatValue` and `""` for NULL.
  - `exportFileStamp`: `sys::localLogStamp()` is `YYYY-MM-DD HH:MM:SS`; drop `-` and `:` and turn the space into `-`.
  - `exportCsv`: `std::filesystem` with `fs::u8path`; `create_directories(dir / "exports")`, open the file `std::ios::binary`,
    write, check the stream; the reported path is `fs::absolute(file).lexically_normal().u8string()`; any failure
    gives `ok=false` and `error = <reason>`.
  - `History::step`: use `std::int64_t` arithmetic, clamp to `[0, size]`.
  - `ScrollState`: `maxTop = max(0, count - height)`; every setter ends in `clamp()`; `ensureVisible(row)`:
    `top = row` if above, `top = row - height + 1` if below.
  - `LogBuffer`: `revision_` increments in `add` and `clear`; `generation_` increments in `clear` and whenever `add` pops entries.
  - `LogBuffer::add` appends the entry, adds its line count to `lines_`, then pops front entries while `lines_ > kMaxLines`
    (the test adds one-line entries and expects exactly `kMaxLines` lines afterwards).

- [ ] **Step 4: Run `ctest --test-dir cpp/build -R wbtext --output-on-failure`.** Expected: PASS.

- [ ] **Step 5: Commit.**

```
git add cpp/include/meradb/wb_text.h cpp/src/wb_text.cpp cpp/tests/test_wb_text.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the workbench text model: cells, CSV, echo lines, clipping, history and scroll state"
```

---

### Task 4: `TextBuffer`: the editor model

**Files:**
- Create: `cpp/include/meradb/wb_editor.h`, `cpp/src/wb_editor.cpp`, `cpp/tests/test_wb_editor.cpp`
- Modify: the two `CMakeLists.txt` as in Task 3

**Interfaces:**

```cpp
// cpp/include/meradb/wb_editor.h
#pragma once
#include "meradb/wb_text.h"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace meradb::wb {

struct Pos {
    int row = 0;
    int col = 0;  // code points, not bytes or cells
    bool operator==(const Pos& o) const { return row == o.row && col == o.col; }
    bool operator!=(const Pos& o) const { return !(*this == o); }
    bool operator<(const Pos& o) const { return row < o.row || (row == o.row && col < o.col); }
};

enum class Move { Left, Right, Up, Down, Home, End, DocStart, DocEnd, PageUp, PageDown, WordLeft, WordRight };

class TextBuffer {
public:
    TextBuffer() : lines_(1) {}
    void setText(const std::string& text);   // normalises \r\n and \r to \n; cursor to the end; selection cleared
    std::string text() const;                 // lines joined by "\n"
    bool empty() const { return lines_.size() == 1 && lines_[0].empty(); }
    int lineCount() const { return static_cast<int>(lines_.size()); }
    const std::string& line(int row) const { return lines_[static_cast<std::size_t>(row)]; }

    Pos cursor() const { return cursor_; }
    bool hasSelection() const { return anchor_.has_value() && *anchor_ != cursor_; }
    std::pair<Pos, Pos> selection() const;    // ordered (from <= to); only meaningful if hasSelection()
    std::string selectedText() const;         // "" when nothing is selected (tui.py: `selected_text or text`)
    void selectRange(Pos anchor, Pos cursor);
    void selectAll();

    void insert(const std::string& utf8);     // replaces the selection; '\n' splits lines; '\t' -> 4 spaces; other control chars dropped
    void newline() { insert("\n"); }
    void backspace();
    void del();
    void move(Move m, bool extendSelection);
    void moveTo(Pos p, bool extendSelection);

    // The text of the selection if any, else the whole text (what F5 / F6 run).
    std::string runnableText() const { return hasSelection() ? selectedText() : text(); }

private:
    std::vector<std::string> lines_;
    Pos cursor_;
    std::optional<Pos> anchor_;
    int desiredCol_ = 0;  // the column Up / Down try to return to
    void deleteSelection();
    Pos clampPos(Pos p) const;
};

// ---- what the view draws ----
int gutterWidth(int lineCount);                          // digits of lineCount (min 2) + 1
std::string gutterText(int row, int lineCount);          // 1-based row number, right aligned, plus a space
int displayColumn(const std::string& line, int cpCol);   // terminal cells before code point column cpCol
// One row of the editor body: highlighted, the selection on a lighter background, and (when `focused`) the cursor
// as an inverse cell (a space past the end of the line).
Line editorRowLine(const TextBuffer& buffer, int row, bool focused);

}  // namespace meradb::wb
```

- [ ] **Step 1: Write the failing tests** `test_wb_editor.cpp` (names start `wbeditor`). Required cases:

```cpp
TEST_CASE("wbeditor setText normalises newlines and puts the cursor at the end", "[wbeditor]") {
    TextBuffer b;
    b.setText("a\r\nbc\rd");
    CHECK(b.lineCount() == 3);
    CHECK(b.text() == "a\nbc\nd");
    CHECK(b.cursor() == Pos{2, 1});
    b.setText("");
    CHECK(b.empty());
    CHECK(b.cursor() == Pos{0, 0});
}
TEST_CASE("wbeditor insert, newline, backspace and delete", "[wbeditor]") {
    TextBuffer b;
    b.insert("abc");
    b.move(Move::Left, false);
    b.newline();                       // ab | c
    CHECK(b.text() == "ab\nc");
    CHECK(b.cursor() == Pos{1, 0});
    b.backspace();                     // joins the lines
    CHECK(b.text() == "abc");
    CHECK(b.cursor() == Pos{0, 2});
    b.del();                           // removes 'c'
    CHECK(b.text() == "ab");
    b.move(Move::DocStart, false);
    b.backspace();                     // nothing before the start
    CHECK(b.text() == "ab");
    b.insert("x\ny\tz\x01");           // tab -> 4 spaces, control dropped, newline splits
    CHECK(b.text() == "x\ny    zab");
}
TEST_CASE("wbeditor code points are columns", "[wbeditor]") {
    TextBuffer b;
    b.insert("\xC3\xA9\xF0\x9F\x98\x80x");   // é 😀 x
    CHECK(b.cursor() == Pos{0, 3});
    b.backspace();
    b.backspace();
    CHECK(b.text() == "\xC3\xA9");
    CHECK(displayColumn("\xC3\xA9\xE6\x97\xA5x", 2) == 3);   // é(1) 日(2) before x
}
TEST_CASE("wbeditor selection drives runnableText", "[wbeditor]") {
    TextBuffer b;
    b.setText("one;\ntwo;\nthree;");
    CHECK(b.runnableText() == "one;\ntwo;\nthree;");
    b.selectRange(Pos{0, 0}, Pos{1, 4});
    CHECK(b.hasSelection());
    CHECK(b.selectedText() == "one;\ntwo;");
    CHECK(b.runnableText() == "one;\ntwo;");
    b.insert("X");                           // typing replaces the selection
    CHECK(b.text() == "X\nthree;");
    CHECK_FALSE(b.hasSelection());
    b.selectAll();
    CHECK(b.selectedText() == "X\nthree;");
    b.move(Move::Left, false);               // collapses to the start of the selection
    CHECK(b.cursor() == Pos{0, 0});
    CHECK_FALSE(b.hasSelection());
}
TEST_CASE("wbeditor shift-moves extend, plain moves clear", "[wbeditor]") {
    TextBuffer b;
    b.setText("abcd\nefgh");
    b.moveTo(Pos{0, 1}, false);
    b.move(Move::Right, true);
    b.move(Move::Right, true);
    CHECK(b.selectedText() == "bc");
    b.move(Move::Down, true);
    CHECK(b.selectedText() == "bcd\nef");
    b.move(Move::Right, false);
    CHECK_FALSE(b.hasSelection());
    b.move(Move::End, true);
    CHECK(b.cursor() == Pos{1, 4});
}
TEST_CASE("wbeditor Up and Down remember the wanted column", "[wbeditor]") {
    TextBuffer b;
    b.setText("abcdef\nab\nabcdef");
    b.moveTo(Pos{0, 5}, false);
    b.move(Move::Down, false);
    CHECK(b.cursor() == Pos{1, 2});
    b.move(Move::Down, false);
    CHECK(b.cursor() == Pos{2, 5});
    b.move(Move::Left, false);     // a horizontal move resets the wanted column
    b.move(Move::Up, false);
    CHECK(b.cursor() == Pos{1, 2});
}
TEST_CASE("wbeditor word moves", "[wbeditor]") {
    TextBuffer b;
    b.setText("foo bar_1  baz");
    b.moveTo(Pos{0, 0}, false);
    b.move(Move::WordRight, false);
    CHECK(b.cursor() == Pos{0, 3});
    b.move(Move::WordRight, false);
    CHECK(b.cursor() == Pos{0, 9});
    b.move(Move::WordLeft, false);
    CHECK(b.cursor() == Pos{0, 4});
}
TEST_CASE("wbeditor row rendering: gutter, selection and cursor", "[wbeditor]") {
    TextBuffer b;
    b.setText("DIKHAO x");
    CHECK(gutterWidth(1) == 3);
    CHECK(gutterWidth(120) == 4);
    CHECK(gutterText(0, 1) == " 1 ");
    b.selectRange(Pos{0, 0}, Pos{0, 3});
    Line row = editorRowLine(b, 0, true);              // the cursor is at column 3 (the selection's end)
    CHECK(plainText(row) == "DIKHAO x");
    CHECK(row[0].text == "DIK");
    CHECK(row[0].style.bg == palette::kCurrentLine);
    CHECK(row[1].text == "H");
    CHECK(row[1].style.inverse);
    b.moveTo(Pos{0, 8}, false);                        // cursor past the end: one extra inverse cell
    Line end = editorRowLine(b, 0, true);
    CHECK(plainText(end) == "DIKHAO x ");
    CHECK(end.back().style.inverse);
    for (const Segment& s : editorRowLine(b, 0, false)) CHECK_FALSE(s.style.inverse);   // unfocused: no cursor
}
```
  Word moves: a word is a run of ASCII letters, digits, `_` and any non-ASCII
  character; `WordRight` skips non-word characters, then the word; `WordLeft` is the mirror; at a line edge it
  steps to the neighbouring line's end/start.

- [ ] **Step 2: Run to see it fail. Step 3: Implement.** Helpers (anonymous namespace):

```cpp
std::size_t cpLen(unsigned char lead) { return lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1; }
int cpCount(const std::string& s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size(); i += std::min(cpLen(static_cast<unsigned char>(s[i])), s.size() - i)) ++n;
    return n;
}
std::size_t byteAt(const std::string& s, int col) {  // byte offset of code point column `col` (clamped to the end)
    std::size_t i = 0;
    while (col > 0 && i < s.size()) {
        i += std::min(cpLen(static_cast<unsigned char>(s[i])), s.size() - i);
        --col;
    }
    return i;
}
```

  Behaviour to implement exactly:
  - `insert(text)`: normalise `\r\n` / `\r` to `\n`, tab to four spaces, drop other bytes < 0x20 and 0x7F;
    `deleteSelection()` first; split at `\n`; the first piece goes into the current line at `byteAt(col)`, the
    text after the cursor is appended to the last piece; cursor ends after the inserted text; `desiredCol_ = col`.
  - `backspace()` / `del()`: with a selection, delete it; else remove one code point (joining lines at a line edge).
  - `move`: `extend && !anchor_` sets `anchor_ = old cursor`; `!extend` clears the anchor, except that plain `Left` /
    `Right` with a selection collapse to the selection's start / end without moving further. `Up` / `Down` use
    `desiredCol_` (clamped to the line); `Up` on row 0 goes to column 0, `Down` on the last row to the line end
    (and then `desiredCol_` is the new column); `PageUp` / `PageDown` move 5 rows; everything except Up / Down /
    Page* updates `desiredCol_`.
  - `editorRowLine`: `highlightLine(line)` split into code-point cells (`std::vector<std::pair<std::string, Style>>`);
    apply `bg = kCurrentLine` to the selected range of this row; if `focused` and the cursor is on this row mark that
    cell `inverse = true` (append a `" "` cell when the cursor is at the end); merge back with `appendSegment`.
  - `gutterWidth(n) = max(2, digits(n)) + 1`; `gutterText` is the 1-based row right-aligned in `gutterWidth-1`
    cells plus one space (`" 1 "`, `"120 "`).

- [ ] **Step 4: Run `ctest -R wbeditor`.** Expected: PASS.

- [ ] **Step 5: Commit.**

```
git add cpp/include/meradb/wb_editor.h cpp/src/wb_editor.cpp cpp/tests/test_wb_editor.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the editor text buffer with selection and highlighted rows"
```

---

### Task 5: `TreeModel`: the schema tree

**Files:**
- Create: `cpp/include/meradb/wb_tree.h`, `cpp/src/wb_tree.cpp`, `cpp/tests/test_wb_tree.cpp`
- Modify: the two `CMakeLists.txt` as in Task 3

**Interfaces:**

```cpp
// cpp/include/meradb/wb_tree.h
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
```

- [ ] **Step 1: Failing tests** `test_wb_tree.cpp` (names `wbtree`), using this JSON:

```cpp
static nlohmann::ordered_json sample() {
    return nlohmann::ordered_json::parse(R"([
      {"name":"college","current":false,"tables":[]},
      {"name":"main","current":true,"tables":[
         {"name":"students","columns":[
            {"name":"id","type_name":"INT","primary_key":true,"unique":false,"not_null":true},
            {"name":"email","type_name":"TEXT","primary_key":false,"unique":true,"not_null":false},
            {"name":"naam","type_name":"TEXT","primary_key":false,"unique":false,"not_null":true}]},
         {"name":"marks","columns":[{"name":"score","type_name":"FLOAT","primary_key":false,"unique":false,"not_null":false}]}]}])");
}
```
  Cases:

  1. Fresh model: `rows()` is one row, label `Databases`, depth 0, expanded.
  2. After `refresh(sample())`: visible rows = `Databases`, `college` (collapsed, expandable although it is empty), `main`
     (expanded), `students`, `marks` (tables collapsed, expandable); the `main` label is bold with
     `fg == palette::kGreen`; `students` label bold.
  3. `allNodes()` has 3 + 4 = 4 databases/tables... compute: root, college, main, students, id, email, naam, marks,
     score = 9 nodes; the column labels as plain text are `id int PK NN`, `email text UQ`, `naam text NN`,
     `score float`; a PK column is not also shown as UQ; the type is lower-cased and coloured `kCyan`, `PK` / `UQ`
     `kOrange`, `NN` dim.
  4. Expansion survives a refresh: `toggle` the `students` row, `refresh(sample())` again -> `students` still expanded
     and its columns visible; a collapsed current database becomes expanded again after a refresh (Python's rule).
  5. Selection survives a refresh by key: select `marks`, refresh -> `rows()[selected()].key` is still `marks`.
  6. `activate`:
     - on `students` with `currentDb == "main"`: `scripts == {"DIKHAO * SE students SIRF 100;"}`, `toggled == true`;
     - on `students` with `currentDb == "college"`: `scripts == {"ISTEMAL main;", "DIKHAO * SE students SIRF 100;"}`;
     - on `college` with `currentDb == "main"`: `scripts == {"ISTEMAL college;"}`, `toggled == true` (an empty database still toggles, as in Textual);
     - on `main` with `currentDb == "main"`: no scripts, `toggled == true`;
     - on a column row `email`: `insertText == "email"`, no scripts;
     - on the root: no scripts, `toggled == true` (the whole tree folds; a second activation unfolds).
  7. `expandOrDescend` / `collapseOrAscend` on `students`, `id`, `main`.
  8. A JSON where a table disappeared: its key is dropped from the expansion set silently (no crash).

- [ ] **Step 2: Implement.** Label building (matching `tui.py`): database label `Segment{name, current ? bold+kGreen : plain}`;
  table label bold; column label = `name`, space, `lower(type_name)` in `kCyan`, then `" PK"` (kOrange) if
  `primary_key`, else `" UQ"` (kOrange) if `unique`, then `" NN"` dim if `not_null`. Use `value("primary_key", false)`
  etc. so older catalogs without a field do not throw. `rebuild()` walks `data_` depth first, emitting rows only
  under expanded parents, then `scroll_.setCount(rows_.size())`. `refresh` computes the new `expanded_` as in the
  header comment, remembers the selected row's key and restores it (or keeps the old index clamped). `activate`
  does: `toggled = row.expandable ? (toggle(row), true) : false` first, then the script / insert rules above (use
  `row.key`, not the row index, after the toggle).

- [ ] **Step 3: Run `ctest -R wbtree`.** Expected: PASS.

- [ ] **Step 4: Commit.**

```
git add cpp/include/meradb/wb_tree.h cpp/src/wb_tree.cpp cpp/tests/test_wb_tree.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the schema tree model with preserved expansion"
```

# BATCH B — the controller and the help text: worker thread, session, Markdown

### Task 6: `Worker` and `Session` core: running text, results, header, log

**Files:**
- Create: `cpp/include/meradb/wb_worker.h`, `cpp/src/wb_worker.cpp`
- Create: `cpp/include/meradb/wb_session.h`, `cpp/src/wb_session.cpp`
- Create: `cpp/tests/wb_test_util.h` (fake backend and manual poster, shared by Tasks 6, 7, 13)
- Create: `cpp/tests/test_wb_worker.cpp`, `cpp/tests/test_wb_session.cpp`
- Modify: `cpp/CMakeLists.txt` (append the two sources to `meradb_workbench`), `cpp/tests/CMakeLists.txt` (append the two tests to `meradb_wb_tests`)

**Interfaces:**

```cpp
// cpp/include/meradb/wb_worker.h
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace meradb::wb {

// ONE thread and a FIFO queue. Every database call of a workbench session runs here: a transaction's statements
// must all run on the thread that ran SHURU, so this is deliberately not a pool.
class Worker {
public:
    Worker();
    ~Worker();  // stopAndJoin()
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool post(std::function<void()> job);   // false once stopAndJoin() has begun
    std::size_t cancelPending();            // drops jobs that have not started; returns how many
    void stopAndJoin();                     // finishes the running job and the queued ones, then joins; idempotent
    std::thread::id threadId() const { return thread_.get_id(); }

private:
    void loop();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
    std::thread thread_;  // declared last: it starts running in the constructor and uses the members above
};

}  // namespace meradb::wb
```

```cpp
// cpp/src/wb_worker.cpp
#include "meradb/wb_worker.h"

namespace meradb::wb {

Worker::Worker() : thread_([this] { loop(); }) {}

Worker::~Worker() { stopAndJoin(); }

bool Worker::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        queue_.push_back(std::move(job));
    }
    wake_.notify_one();
    return true;
}

std::size_t Worker::cancelPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t dropped = queue_.size();
    queue_.clear();
    return dropped;
}

void Worker::stopAndJoin() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) thread_.join();
}

void Worker::loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stopping and drained
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            job();
        } catch (...) {  // a job reports its own failures; nothing may end the thread
        }
    }
}

}  // namespace meradb::wb
```

```cpp
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
    // Task 7:
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
    // ... private members and helpers (see the notes below)
};

}  // namespace meradb::wb
```

  The private part: `Header header_`; `std::unique_ptr<Backend> backend_` (**worker thread only**); `SessionOptions options_`;
  `UiPoster post_`; `TextBuffer editor_`; `LogBuffer log_`; `TreeModel tree_`; `std::optional<ResultTable> table_`;
  `std::optional<Result> lastResult_`; `History history_`; `Modal modal_`; `std::optional<Panel> focusRequest_`;
  `int pending_ = 0`, `int resultVersion_ = 0`; `bool quitting_ = false, shutdown_ = false`; `std::function<void()> onExit_`;
  and last, so it is destroyed first, `Worker worker_`.

- [ ] **Step 1: Write `wb_test_util.h`** (a scripted backend and a stand-in for `ScreenInteractive::Post`):

```cpp
// cpp/tests/wb_test_util.h
#pragma once
#include "meradb/backend.h"
#include "meradb/errors.h"
#include "meradb/wb_session.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace wbtest {

// Stands in for ScreenInteractive::Post: closures queue up and the TEST thread (the "UI thread") runs them.
class ManualPoster {
public:
    meradb::wb::UiPoster poster() {
        return [this](std::function<void()> f) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.push_back(std::move(f));
            }
            wake_.notify_all();
        };
    }
    // Runs posted closures here until done() is true; false on timeout.
    bool pumpUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (done()) return true;
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (queue_.empty() && wake_.wait_until(lock, deadline) == std::cv_status::timeout && queue_.empty())
                    return done();
                if (queue_.empty()) continue;
                f = std::move(queue_.front());
                queue_.pop_front();
            }
            f();
        }
    }
    bool pumpIdle(meradb::wb::Session& s) { return pumpUntil([&s] { return !s.busy(); }); }

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
};

// A Backend whose answers the test scripts. All calls are recorded with the thread they ran on.
class FakeBackend : public meradb::Backend {
public:
    std::map<std::string, std::vector<meradb::Result>> replies;  // by exact statement text; default: message "ok"
    std::function<void(const std::string&)> beforeRun;           // may block (runs on the worker)
    bool dropped = false;      // runScript throws "Server se connection toot gaya"
    bool schemaFails = false;  // schemaTree throws
    bool txn = false;          // set by "SHURU;", cleared by "PAKKA;" / "WAPAS;"
    bool closed = false, rolledBackOnClose = false;
    std::string db = "main", desc = "fake:1";
    nlohmann::ordered_json tree = nlohmann::ordered_json::array();

    std::vector<std::string> ranList() const { std::lock_guard<std::mutex> l(m_); return ran_; }
    std::vector<std::thread::id> threadList() const { std::lock_guard<std::mutex> l(m_); return threads_; }

    std::vector<meradb::Result> runScript(const std::string& text) override {
        { std::lock_guard<std::mutex> l(m_); threads_.push_back(std::this_thread::get_id()); }
        if (beforeRun) beforeRun(text);
        if (dropped) throw meradb::ConnectionFailed("Server se connection toot gaya: fake");
        { std::lock_guard<std::mutex> l(m_); ran_.push_back(text); }
        if (text == "SHURU;") txn = true;
        if (text == "PAKKA;" || text == "WAPAS;") txn = false;
        auto it = replies.find(text);
        if (it != replies.end()) return it->second;
        meradb::Result r;
        r.message = "ok";
        return {r};
    }
    std::vector<meradb::Result> execute(const std::string& text) override { return runScript(text); }
    std::string currentDb() override { return db; }
    bool inTransaction() override { return txn; }
    nlohmann::ordered_json schemaTree() override {
        if (schemaFails) throw meradb::StorageError("schema nahi mila");
        return tree;
    }
    std::string description() override { return desc; }
    void close() override { closed = true; rolledBackOnClose = txn; txn = false; }

private:
    mutable std::mutex m_;
    std::vector<std::string> ran_;
    std::vector<std::thread::id> threads_;
};

inline meradb::Result tableResult(std::vector<std::string> columns, std::vector<std::vector<meradb::Value>> rows) {
    meradb::Result r;
    r.columns = std::move(columns);
    r.rows = std::move(rows);
    return r;
}

}  // namespace wbtest
```

- [ ] **Step 2: Write the failing tests.** `test_wb_worker.cpp` (names `wbworker`): jobs run in FIFO order on a thread other
  than the caller's; `cancelPending` drops only queued jobs (block the first job on a promise, queue two more, cancel,
  release -> only the first ran, `cancelPending()` returned 2); `stopAndJoin` runs the queued jobs, then `post` returns
  false; a job that throws does not stop later jobs; the destructor joins without a hang.

  `test_wb_session.cpp` (names `wbsession`), each building `FakeBackend` (heap, handed to the `Session`, raw pointer
  kept for assertions), `ManualPoster`, `Session s(std::move(fake), opts, poster.poster())`, then `poster.pumpIdle(s)`:

  1. **Start-up:** `s.log().entries()` holds `Bold` "Namaste! Connected: fake:1" then `Dim` "F1 dabao madad ke liye.";
     `s.subtitle() == "fake:1  |  db: main"`; after `pumpIdle` the tree shows the fake's schema (set `fake->tree` before
     constructing: one database, one table).
  2. **A SELECT:** `fake->replies["DIKHAO * SE t;"] = {tableResult({"id","naam"}, {{Value(int64_t(1)), Value(std::string("A"))}})}`;
     `s.runText("  DIKHAO * SE t;  ")` -> after `pumpIdle`: `s.history().items() == {"DIKHAO * SE t;"}`; log kinds after the two
     start-up entries are `Echo`, `Dim` (matching `^\(\d+\.\d ms\)$`); `echo` first line `main> DIKHAO * SE t;`;
     `s.table()` has 1 row and columns `id`, `naam`; `resultsTitle(s.table()) == "Results -- 1 row(s)"`;
     `resultVersion()` is 1.
  3. **Several results:** a script whose replies are `[message "a", error "[Execution Galti] x", table T1, message "b", table T2]`
     -> log lines in order: `Message a`, `Error [Execution Galti] x`, `Message b` (a table result with an empty `message`
     logs nothing), then `(ms)`; the shown table is T2 (the LAST one). A result with an `error` never becomes the table
     even if it has columns.
  4. **Empty text:** `runText("   ")` changes nothing: no history, no log entry, `busy()` stays false, backend not called.
  5. **Duplicate history:** running the same text twice stores it once.
  6. **Dropped connection:** `fake->dropped = true` then `runText("x;")` -> log gets the `Echo`, then `Error` with
     `[Connection Galti] Server se connection toot gaya: fake`, then `Dim` `Ctrl+O se dobara connect karo.`; NO `(ms)`
     line; the previous table and the header are unchanged.
  7. **Transaction indicator:** `runText("SHURU;")` -> `s.inTransaction()`, `subtitle()` ends with
     `  |  TRANSACTION (PAKKA / WAPAS)`; `runText("WAPAS;")` removes it.
  8. **Schema failure:** `fake->schemaFails = true` -> after a run the log has a `Dim` entry
     `(schema refresh nahi hua: [Storage Galti] schema nahi mila)` and the tree is unchanged.
  9. **Thread affinity:** after 3 runs `fake->threadList()` has 3 equal ids, different from `std::this_thread::get_id()`.
  10. **Busy and queueing:** `fake->beforeRun` blocks on a `std::promise<void>`'s future for the first statement; `runText("a;")`
      then `runText("b;")` -> `s.busy()`, `s.pendingJobs() == 2`, `busyLabel() == "[chal raha hai +1]"`; the echo lines of
      both are already in the log (echo is immediate); release; `pumpIdle`; `ranList() == {"a;", "b;"}` (initial schema load
      does not run a statement); `busyLabel() == ""`.
  11. **Selection-aware run:** `s.editor().setText("one;\ntwo;")`, `selectRange({0,0},{0,4})`, `runEditorText()` -> `ranList() == {"one;"}`;
      with no selection the whole text `"one;\ntwo;"` runs.
  12. **clearLog:** `clearLog()` empties `log().entries()`.

- [ ] **Step 3: Implement** `wb_worker.cpp` as above, and `Session` core (`wb_session.cpp`):

  - Constructor: store options (default the factory and stamp), `header_ = {backend->description(), backend->currentDb(),
    backend->inTransaction()}` (these are plain field reads for both backends and happen before the thread exists),
    log `Bold` `Namaste! Connected: <description>` and `Dim` `F1 dabao madad ke liye.`, `backend_ = std::move(backend)`, then
    `++pending_` and post a job that calls `takeSnapshot()` and posts `applySnapshotOnly`. (`worker_` is constructed last,
    after `backend_` is set: initialise members in declaration order and start the first job from the constructor body.)
  - `takeSnapshot()` (worker thread): reads `description()`, `currentDb()`, `inTransaction()`; `schemaTree()` inside
    `try { ... } catch (const std::exception& e) { schemaError = e.what(); }`.
  - `runText(raw)`: if `quitting_` return; `text = pytext::strip(raw)`; empty -> return; `history_.add(text)`;
    `log_.add(echoEntry(header_.db, text))`; `enqueueRun(text, std::nullopt)`.
  - `enqueueRun(text, echoAtCompletion)`: `++pending_`; post a job that fills a `RunOutcome` exactly like this and
    posts the apply closure:

```cpp
worker_.post([this, text, echo] {
    RunOutcome out;
    out.echo = echo;
    const auto started = std::chrono::steady_clock::now();
    try {
        out.results = backend_->runScript(text);
        out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        out.snapshot = takeSnapshot();
    } catch (const std::exception& e) {  // MeraDBError: the server went away; anything else is reported the same way
        out.failed = true;
        out.failure = e.what();
    }
    post_([this, out]() mutable { --pending_; applyRun(std::move(out)); });
});
```

  - `applyRun(out)`, in this order (tui.py `run_text`): if `out.echo`: `history_.add(*out.echo)` and echo entry; if `out.notice` (Task 7):
    log it and return; if `out.failed`: `Error` + `Dim "Ctrl+O se dobara connect karo."`, return (no ms line, no schema, no header);
    loop results: error -> `Error`; else remember the last result with columns, and a non-empty message -> `Message`;
    then `Dim` `snprintf("(%.1f ms)")`; if a table result exists `showResult(it)`; `applySnapshot(out.snapshot)`.
  - `showResult(r)`: `lastResult_ = r; table_ = makeTable(r); ++resultVersion_`.
  - `applySnapshot(s)`: `header_ = {s.description, s.db, s.inTransaction}`; schema error -> `Dim` `(schema refresh nahi hua: <e>)` and the
    tree is left alone; else `tree_.refresh(s.schema)`.
  - `subtitle()`, `busyLabel()` (`quitting_` -> `[band ho raha hai ...]`; `pending_ == 0` -> empty; `1` -> `[chal raha hai]`;
    `n` -> `[chal raha hai +<n-1>]`), `takeFocusRequest()`, `clearLog()`, `logLine()`, `runEditorText()` (`runText(editor_.runnableText())`).
  - `connectDefaults(description)`: if it starts with `local (` or has no `:` -> `{"127.0.0.1","6372"}`; else split at the LAST `:`; the
    port part must be all digits, else the defaults. `makeConnectRequest`: `pytext::strip` each value; defaults `127.0.0.1` / `6372`; empty
    password / database -> `std::nullopt`.
  - The remaining public methods are stubs that Task 7 fills (declare them now so the header is final).

- [ ] **Step 4: Run `ctest -R "wbworker|wbsession" --output-on-failure`.** Expected: PASS (Task 7's tests are not written yet).
  Run the session tests 20 times in a loop (`--repeat until-fail:20`) to shake out races; a flaky test is a bug in the
  test or the code, never retry-and-ignore.

- [ ] **Step 5: Commit.**

```
git add cpp/include/meradb/wb_worker.h cpp/src/wb_worker.cpp cpp/include/meradb/wb_session.h cpp/src/wb_session.cpp cpp/tests/wb_test_util.h cpp/tests/test_wb_worker.cpp cpp/tests/test_wb_session.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the workbench worker thread and the session core that runs statements off the UI thread"
```

---

### Task 7: `Session` actions: explain, history, export, connect, quit, shutdown

**Files:**
- Modify: `cpp/src/wb_session.cpp`, `cpp/include/meradb/wb_session.h` (only if a declaration was missing)
- Modify: `cpp/tests/test_wb_session.cpp` (append cases 13-24)

- [ ] **Step 1: Append the failing tests** (continue numbering; same fixtures):

  13. **Explain, one statement:** editor `DIKHAO * SE t;` -> `explainEditorText()` -> backend ran `SAMJHAO DIKHAO * SE t;`; log has an `Echo`
      `main> SAMJHAO DIKHAO * SE t;`; `history().items().back() == "SAMJHAO DIKHAO * SE t;"`.
  14. **Explain, two statements:** editor `DIKHAO * SE a; DIKHAO * SE b;` -> log gets a `Warn`
      `SAMJHAO ek hi query par chalta hai -- ek query select karke F6 dabao`; backend not called; no history entry.
  15. **Explain, empty or only a comment:** the same `Warn` (zero statements). **Explain, bad syntax** (`DIKHAO FROM;` or any text `parseScript`
      rejects): an `Error` entry whose text starts with `[` and ends with the parser's message; backend not called; no history.
  16. **Explain is selection-aware:** only the selected statement is explained.
  17. **History:** after running `a;` then `b;`: `historyStep(-1)` -> `editor().text() == "b;"` and `takeFocusRequest() == Panel::Editor`
      (a second `takeFocusRequest()` is empty); `historyStep(-1)` -> `"a;"`; `historyStep(1)` -> `"b;"`; `historyStep(1)` -> `""`.
      With an empty history `historyStep(-1)` changes nothing and requests no focus.
  18. **Export:** before any result: a `Warn` `Pehle koi DIKHAO query chalao, phir Ctrl+S` and no file. After a 2-row result with
      `exportBaseDir = TempDir` and `stamp = "20260101-000000"`: the file `<tmp>/exports/meradb-20260101-000000.csv` holds the
      CSV bytes and the log's last entry is a `Message` `2 row(s) CSV mein save: <absolute path>`. An unwritable target
      (a regular file named `exports`) logs an `Error` starting `CSV save nahi hua: `.
  19. **Connect dialog helpers:** `makeConnectRequest(false, " h ", "", "", " db ")` -> host `h`, port `6372`, no password, database `db`;
      `makeConnectRequest(true, "", "", "pw", "")` -> local, host `127.0.0.1`, password `pw`; `connectDefaults("10.0.0.5:6400")` ->
      `{10.0.0.5, 6400}`; `connectDefaults("local (C:\\data)")` -> defaults; `connectDefaults("::1:6372")` -> `{::1, 6372}`;
      `defaultBackendFactory` with port `abc` throws `std::invalid_argument` whose message is
      `invalid literal for int() with base 10: 'abc'`. `openConnectDialog()` sets `modal() == Modal::Connect`; `closeModal()` clears it.
  20. **Connect success:** a factory returning a second `FakeBackend` (`desc = "other:2"`, `db = "college"`) -> after `connect(...)` and `pumpIdle`:
      `subtitle() == "other:2  |  db: college"`; the log's last `Connected` entry is `Connected: other:2`; the OLD backend is `closed`
      (and `rolledBackOnClose` if it was in a transaction: run `SHURU;` on it first); the tree shows the new schema; the dialog is closed;
      every later statement runs on the NEW backend, still on one thread.
  21. **Connect failure:** the factory throws `meradb::ConnectionFailed("kaun hai")` -> an `Error` `Connect nahi hua: [Connection Galti] kaun hai`;
      the old backend is untouched (not closed, still used by the next statement), header unchanged.
  22. **Tree activation:** with the tree loaded from a fake schema (database `main` current, table `t`): `activateTreeRow(<row of t>)` queues
      `DIKHAO * SE t SIRF 100;` (db is current, so no `ISTEMAL`); for a column row it inserts the column name at the editor cursor and
      requests the Editor focus; for another database row it runs `ISTEMAL <db>;`. Expansion toggles as in Task 5.
  23. **Quit when idle:** `requestQuit()` -> after `pump`, the exit callback was called exactly once, the backend `closed` (rolled back if in a
      transaction), `quitting()` is true, `busyLabel() == "[band ho raha hai ...]"` until then; further `runText` calls are ignored;
      a second `requestQuit()` does nothing.
  24. **Quit while busy:** the first statement blocks in `beforeRun`; queue a second; `requestQuit()` -> the second is dropped (never run),
      `onExit` has NOT been called while the first is blocked; after releasing it, `onExit` is called once, `ranList() == {"first;"}`,
      `closed == true`. **Shutdown without quit:** destroying the session (or `shutdown()`) closes the backend and returns even
      while the UI never pumps; calling it twice is harmless; after `shutdown()` no posted closure touches the session
      (the poster is simply never pumped again).

- [ ] **Step 2: Run to see failures. Step 3: Implement.**

  - `explainEditorText()`: `quitting_` -> return; `text = strip(editor_.runnableText())`; `++pending_`; job on the worker:

```cpp
RunOutcome out;
try {
    const auto statements = parseScript(text);               // MeraDBError on bad syntax
    if (statements.size() != 1) {
        out.notice = std::make_pair(LogKind::Warn, std::string("SAMJHAO ek hi query par chalta hai -- ek query select karke F6 dabao"));
    } else {
        const std::string full = "SAMJHAO " + text;
        out.echo = full;
        runOnWorker(full, out);                              // the same body as enqueueRun's job: results, ms, snapshot or failure
    }
} catch (const MeraDBError& e) {
    out.notice = std::make_pair(LogKind::Error, std::string(e.what()));
}
post_([this, out]() mutable { --pending_; applyRun(std::move(out)); });
```

    Factor the body of `enqueueRun`'s job into `runOnWorker(const std::string& text, RunOutcome& out)` so both paths share it. `RunOutcome` gets
    `std::optional<std::pair<LogKind, std::string>> notice`.
  - `historyStep`: `if (auto t = history_.step(delta)) { editor_.setText(*t); focusRequest_ = Panel::Editor; }`.
  - `exportCsv`: described by the test (uses `options_.stamp()` and `options_.exportBaseDir`; success message
    `<n> row(s) CSV mein save: <path>` with `n = lastResult_->rows.size()`).
  - `openConnectDialog()` / `showHelp()` set `modal_`; `closeModal()` clears; `connect(req)`: `modal_ = None`; if `quitting_` return; `++pending_`;
    worker job:

```cpp
ConnectOutcome out;
try {
    std::unique_ptr<Backend> fresh;
    if (request.local && dynamic_cast<LocalBackend*>(backend_.get()) != nullptr) {
        closeBackendOnWorker();                              // R14: never two engines on one folder
    }
    fresh = options_.factory(request, options_.dataDir);     // may throw (MeraDBError, std::invalid_argument, ...)
    std::unique_ptr<Backend> old = std::move(backend_);
    backend_ = std::move(fresh);
    try { if (old) old->close(); } catch (...) {}
    out.snapshot = takeSnapshot();
} catch (const std::exception& e) {
    out.error = e.what();
}
post_([this, out]() { --pending_; applyConnect(out); });
```

    `applyConnect`: error -> `Error` `Connect nahi hua: <error>`; success -> `Connected` entry `Connected: <description>` (bold green) then
    `applySnapshot`. (If a local backend was closed first and the factory then fails, the old engine object is closed but usable; acceptable.)
  - `activateTreeRow(row)`: `act = tree_.activate(row, header_.db)`; for each script `runText(script)` in order; if `insertText` is not empty:
    `editor_.insert(act.insertText); focusRequest_ = Panel::Editor`.
  - `requestQuit()` / `shutdown()`:

```cpp
void Session::requestQuit() {
    if (quitting_ || shutdown_) return;
    quitting_ = true;
    pending_ -= static_cast<int>(worker_.cancelPending());   // those jobs will never post back
    worker_.post([this] {
        closeBackendOnWorker();
        post_([this] { if (onExit_) onExit_(); });
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
}
```

    `closeBackendOnWorker()`: `if (backend_) { try { backend_->close(); } catch (...) {} backend_.reset(); }` (the engine's `close` rolls back an
    open transaction on this same thread, and the backend is destroyed here, on the thread that used it). After `quitting_` every action
    (`runText`, `explain`, `connect`, `historyStep` is harmless) returns at once.

- [ ] **Step 4: Run the whole `wbsession` set 20 times** (`ctest -R wbsession --repeat until-fail:20`). Expected: PASS every time.

- [ ] **Step 5: Commit.**

```
git add cpp/src/wb_session.cpp cpp/include/meradb/wb_session.h cpp/tests/test_wb_session.cpp
git commit -m "Add explain, history, CSV export, connect, tree activation and safe quit to the workbench session"
```

---

### Task 8: Help: Markdown subset, embedded `LANGUAGE.md`, key table

**Files:**
- Create: `cpp/cmake/embed_file.cmake`
- Create: `cpp/include/meradb/wb_markdown.h`, `cpp/src/wb_markdown.cpp`
- Create: `cpp/include/meradb/wb_help.h`, `cpp/src/wb_help.cpp`
- Create: `cpp/tests/test_wb_help.cpp`
- Modify: `cpp/CMakeLists.txt` (generated file rule, sources, include dir), `cpp/tests/CMakeLists.txt` (test file, `MERADB_LANGUAGE_MD` definition)

**Interfaces:**

```cpp
// cpp/include/meradb/wb_markdown.h
#pragma once
#include "meradb/wb_text.h"
#include <string>
#include <vector>

namespace meradb::wb {
// The small Markdown subset docs/LANGUAGE.md and the key table use, as styled terminal lines for a given width.
// Blocks: # .. ###### headings (bold; level 1 pink, 2 cyan, 3+ plain bold), paragraphs (word-wrapped, lines of a
// paragraph joined by one space), "- " / "* " bullets (shown "• ", hanging indent), "1. " numbered items (kept),
// fenced ``` code blocks (verbatim, never wrapped, on the current-line background), tables (| a | b | with a
// |---| divider: aligned columns joined by " │ ", header bold, a "─┼─" divider line), "> " quotes (dim, "▌ "),
// horizontal rules (--- or ***). One blank line between blocks. Inline: **bold**, `code` (cyan), *italic*.
// CRLF and LF both work. Never throws; a width below 1 counts as 1.
std::vector<Line> renderMarkdown(const std::string& markdown, int width);
}  // namespace meradb::wb
```

```cpp
// cpp/include/meradb/wb_help.h
#pragma once
#include "meradb/wb_text.h"
#include <string>
#include <vector>

namespace meradb::wb {
const char* keysHelpMarkdown();                         // tui.py KEYS_HELP, verbatim, plus the C++ extra keys section
bool languageDocEmbedded();                             // false when docs/LANGUAGE.md was missing at build time
std::string languageDoc();                              // the embedded text, or "*(docs/LANGUAGE.md nahi mila)*"
std::string helpMarkdownWith(const std::string& language);   // keysHelpMarkdown() + "\n" + language
std::string helpMarkdown();                             // helpMarkdownWith(languageDoc())
std::vector<Line> renderHelp(int width);                // renderMarkdown(helpMarkdown(), width)
}  // namespace meradb::wb
```

- [ ] **Step 1: The embed script** `cpp/cmake/embed_file.cmake` (CMake regular expressions have no `{n}` repeat, so the byte
  list is one long line; compilers do not mind):

```cmake
# cpp/cmake/embed_file.cmake -- cmake -DINPUT=<file> -DOUTPUT=<file.inc> -P embed_file.cmake
# Turns a text file into a C++ byte array so the workbench can show docs/LANGUAGE.md without finding it at run time.
if(EXISTS "${INPUT}")
  file(READ "${INPUT}" _hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
  set(_present "true")
else()
  set(_bytes "")
  set(_present "false")
endif()
file(WRITE "${OUTPUT}"
  "// GENERATED by cmake/embed_file.cmake; do not edit.\n"
  "static const unsigned char kLanguageDocBytes[] = {${_bytes}0x00};\n"
  "static const bool kLanguageDocPresent = ${_present};\n")
```

  In `cpp/CMakeLists.txt`, inside `if(MERADB_WORKBENCH)` before `add_library(meradb_workbench ...)`:

```cmake
  set(MERADB_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
  set(MERADB_LANGUAGE_MD ${CMAKE_CURRENT_SOURCE_DIR}/../docs/LANGUAGE.md)
  add_custom_command(
    OUTPUT ${MERADB_GEN_DIR}/language_doc_data.inc
    COMMAND ${CMAKE_COMMAND} -DINPUT=${MERADB_LANGUAGE_MD} -DOUTPUT=${MERADB_GEN_DIR}/language_doc_data.inc
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
    DEPENDS ${MERADB_LANGUAGE_MD} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
    COMMENT "Embedding docs/LANGUAGE.md"
    VERBATIM)
```

  and add `${MERADB_GEN_DIR}/language_doc_data.inc` to the `meradb_workbench` source list (listing the generated file
  makes every generator run the rule), `src/wb_markdown.cpp`, `src/wb_help.cpp`, and
  `target_include_directories(meradb_workbench PRIVATE ${MERADB_GEN_DIR})`. If `docs/LANGUAGE.md` does not exist the
  `DEPENDS` on a missing file fails the build: wrap the `DEPENDS` file in `if(EXISTS ...)` (CMake: build the argument list
  conditionally) so a source tree without `docs/` still builds with the fallback text.

- [ ] **Step 2: Write the failing tests** `test_wb_help.cpp` (names `wbhelp`). Helper `plain(lines)` returns the vector of
  `plainText`. Cases (each exact):

  - `renderMarkdown("# Title\n\nalpha beta gamma delta", 12)` -> `{"Title", "", "alpha beta", "gamma delta"}`; the first line's segment is
    bold with `fg == palette::kPink`.
  - `renderMarkdown("## Sub\ntext", 40)` -> `{"Sub", "", "text"}`... (a heading is a block; the next block follows after one blank line).
  - `renderMarkdown("- **x** and `y`\n- second", 40)` -> `{"• x and y", "• second"}`; `x` is bold, `y` has `fg == palette::kCyan`.
  - Hanging indent: `renderMarkdown("- aaa bbb ccc", 8)` -> `{"• aaa", "  bbb", "  ccc"}`.
  - Table:

```
| Key | Kaam |
|-----|------|
| **F5** | chalao |
```
    -> `{"Key │ Kaam", "────┼───────", "F5  │ chalao"}`; the header cells are bold, `F5` is bold.
  - Fence: ```` ```\nSELECT 1;\n  x\n``` ```` -> `{"SELECT 1;", "  x"}`, every segment on `bg == palette::kCurrentLine`; a code line longer than the
    width is NOT wrapped.
  - Quote and rule: `"> hi"` -> `{"▌ hi"}` dimmed; `"---"` at width 5 -> `{"─────"}`.
  - CRLF input gives the same lines as LF input. Width 1 and an empty string do not crash or loop (`renderMarkdown("", 10)` is empty).
  - `keysHelpMarkdown()` starts with `# MeraDB Workbench: Madad`, contains the row `| **F5** / **Ctrl+R** | Query chalao (selected text only, if something is selected) |`
    and the Hinglish rows of `tui.py` verbatim, and contains `## Extra keys (C++ version)`.
  - `helpMarkdownWith("X")` ends with `\nX`; `helpMarkdownWith("*(docs/LANGUAGE.md nahi mila)*")` shows that line when rendered (fallback path).
  - Integration: `languageDocEmbedded()` is true in this repository; `languageDoc()` equals `docs/LANGUAGE.md` after removing `\r`
    (the test reads the path from the compile definition `MERADB_LANGUAGE_MD`); `renderHelp(80)` has a line equal to
    `MeraDB Query Language (MQL): Reference`, a line containing `Keyword ↔ SQL cheat sheet`, a table row containing `BANAO` and `CREATE`, and
    NO line containing `**` or starting with `|`.

- [ ] **Step 3: Implement.** `wb_help.cpp` includes the generated file (`#include "language_doc_data.inc"`) and builds
  `std::string(reinterpret_cast<const char*>(kLanguageDocBytes), sizeof(kLanguageDocBytes) - 1)`. The key table, verbatim from
  `tui.py` `KEYS_HELP`, then the extra section (a raw string literal with a custom delimiter, `R"MD(...)MD"`):

```
# MeraDB Workbench: Madad

| Key | Kaam |
|-----|------|
| **F5** / **Ctrl+R** | Query chalao (selected text only, if something is selected) |
| **F6** | SAMJHAO: query plan dikhao (index / scan / join), bina chalaye |
| **Ctrl+Up / Ctrl+Down** | Pichli / agli query (history) |
| **Ctrl+S** | Results ko CSV file mein save karo (`exports/` folder) |
| **Ctrl+O** | Doosre server se connect karo, ya local mode |
| **Tab / Shift+Tab** | Editor, results aur schema tree ke beech jao |
| **Enter** on a table in the tree | Uske pehle 100 rows dikhao |
| **Enter** on a database | Us database ko ISTEMAL karo |
| **Enter** on a column | Column ka naam editor mein daalo |
| **Ctrl+L** | Log saaf karo |
| **F1** / **Esc** | Ye madad kholo / band karo |
| **Ctrl+Q** | Bahar niklo |

## Extra keys (C++ version)

| Key | Kaam |
|-----|------|
| **Ctrl+P / Ctrl+N** | History: pichli / agli (jab Ctrl+Up / Ctrl+Down terminal se na aaye) |
| **Shift+Arrows / Home / End** | Text select karo; F5 / F6 sirf select kiya hua hissa chalate hain |
| **Ctrl+A** | Editor ka poora text select karo |
| **PageUp / PageDown** | Results, log, tree aur madad mein scroll karo |
| **Mouse** | Panel par click karo (focus), wheel se scroll |
```

  (Copy the first table from `meradb/tui.py` again when implementing, byte for byte; the two lines above are what it
  contained when this plan was written.) The Markdown renderer: process line by line with a block state machine
  (fence / table / list / quote / paragraph); inline parsing is a left-to-right scan for `**`, `` ` ``, `*` pairs
  (an unmatched marker is literal text); a paragraph is wrapped by `wrapWords(Line, width)`: break at the last
  space that fits, hard-break words longer than the width, drop the space at a break. Table cell widths use
  `ftxui::string_width`, column joiner `" │ "` (U+2502), divider `─` (U+2500) and `┼` (U+253C), bullet `•` (U+2022),
  quote bar `▌` (U+258C).

- [ ] **Step 4: Run `ctest -R wbhelp`.** Expected: PASS. Then `cmake --build cpp/build` once more after `touch docs/LANGUAGE.md`
  to confirm the rule re-embeds (the build prints "Embedding docs/LANGUAGE.md"); restore the mtime is not needed.

- [ ] **Step 5: Commit.**

```
git add cpp/cmake/embed_file.cmake cpp/include/meradb/wb_markdown.h cpp/src/wb_markdown.cpp cpp/include/meradb/wb_help.h cpp/src/wb_help.cpp cpp/tests/test_wb_help.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the help screen text: a small Markdown renderer and the embedded language reference"
```

# BATCH C — the FTXUI view and the program entry

FTXUI facts this batch relies on (v5.0.0 headers, checked): `ScreenInteractive::Fullscreen()`, `Loop`, `Post(Task)`,
`PostEvent`, `ExitLoopClosure`, `TrackMouse(bool)`; `Event::ArrowUpCtrl/DownCtrl/LeftCtrl/RightCtrl`, `F1..F12`, `Tab`,
`TabReverse`, `Escape`, `Return`, `Backspace`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Event::Special(std::string)`,
`Event::Character(std::string)`, `event.input()`, `event.is_character()`, `event.character()`, `event.is_mouse()`,
`event.mouse()`; `window(title, content)` (no border-style parameter), `borderStyled`, `reflect(Box&)`, `clear_under`,
`dbox`, `size(WIDTH|HEIGHT, EQUAL|LESS_THAN|GREATER_THAN, n)`, `color`, `bgcolor`, `bold`, `dim`, `italic`, `underlined`,
`inverted`, `Color::RGB`, `Screen::Create(Dimension::Fixed(w), Dimension::Fixed(h))`, `Render(Screen&, Element)`,
`Screen::PixelAt(x, y)` with `character`, `bold`, `dim`, `italic`, `underlined`, `inverted`, `foreground_color`,
`background_color`, and `Color::Print(bool isBackground)` (a stable string to compare colours with). Components are
`ComponentBase` subclasses (`Render()`, `OnEvent(Event)`); this view drives its panels directly and does **not**
use FTXUI's focus tree (we route keys and focus ourselves, as Python's priority bindings do). Anything not in this list:
check the header before relying on it.

### Task 9: FTXUI panels: tree, results, log, editor

**Files:**
- Create: `cpp/include/meradb/wb_keys.h`
- Create: `cpp/include/meradb/wb_panels.h`, `cpp/src/wb_panels.cpp`
- Create: `cpp/tests/wb_screen_util.h`, `cpp/tests/test_wb_panels.cpp`
- Modify: the two `CMakeLists.txt` (append `src/wb_panels.cpp`, `test_wb_panels.cpp`)

**Interfaces:**

```cpp
// cpp/include/meradb/wb_keys.h -- what each key means. The only place that knows event bytes.
#pragma once
#include <ftxui/component/event.hpp>
#include <string>

namespace meradb::wb::keys {

inline ftxui::Event ctrl(char letter) { return ftxui::Event::Special(std::string(1, static_cast<char>(letter & 0x1f))); }

// Shift + arrows / Home / End as xterm-style terminals (and Windows Terminal) send them: ESC [ 1 ; 2 <final>.
inline const std::string kShiftUp = "\x1b[1;2A", kShiftDown = "\x1b[1;2B", kShiftRight = "\x1b[1;2C",
                         kShiftLeft = "\x1b[1;2D", kShiftHome = "\x1b[1;2H", kShiftEnd = "\x1b[1;2F";

inline bool isRun(const ftxui::Event& e) { return e == ftxui::Event::F5 || e == ctrl('R'); }
inline bool isExplain(const ftxui::Event& e) { return e == ftxui::Event::F6; }
inline bool isHistoryPrev(const ftxui::Event& e) { return e == ftxui::Event::ArrowUpCtrl || e == ctrl('P'); }
inline bool isHistoryNext(const ftxui::Event& e) { return e == ftxui::Event::ArrowDownCtrl || e == ctrl('N'); }
inline bool isExport(const ftxui::Event& e) { return e == ctrl('S'); }
inline bool isConnect(const ftxui::Event& e) { return e == ctrl('O'); }
inline bool isClearLog(const ftxui::Event& e) { return e == ctrl('L'); }
inline bool isHelp(const ftxui::Event& e) { return e == ftxui::Event::F1; }
inline bool isQuit(const ftxui::Event& e) { return e == ctrl('Q'); }
inline bool isInterrupt(const ftxui::Event& e) { return e == ctrl('C'); }
inline bool isSelectAll(const ftxui::Event& e) { return e == ctrl('A'); }

}  // namespace meradb::wb::keys
```

```cpp
// cpp/include/meradb/wb_panels.h
#pragma once
#include "meradb/wb_session.h"
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>

namespace meradb::wb {

ftxui::Element lineToElement(const Line& line);   // one styled line: an hbox of coloured text pieces

// A bordered panel with a title; focused panels get the yellow border (tui.py: `border: heavy $warning`).
ftxui::Element panelFrame(const std::string& title, ftxui::Element content, bool focused, int accentRgb);

class PanelView : public ftxui::ComponentBase {
public:
    void setFocused(bool focused) { focused_ = focused; }
    bool focused() const { return focused_; }
    const ftxui::Box& box() const { return box_; }   // where the panel was drawn last frame (mouse hit-tests)
    bool Focusable() const override { return true; }
    // Mouse support (Task 11): a wheel step or click at a position inside box().
    virtual void scrollWheel(int /*direction*/) {}
protected:
    bool focused_ = false;
    ftxui::Box box_;
    int innerHeight() const;                         // rows inside the frame, from the previous frame's box_
    int innerWidth() const;
};

std::shared_ptr<PanelView> makeTreePanel(Session& session);      // "Schema"
std::shared_ptr<PanelView> makeResultsPanel(Session& session);   // title from resultsTitle()
std::shared_ptr<PanelView> makeLogPanel(Session& session);       // "Log"
std::shared_ptr<PanelView> makeEditorPanel(Session& session);    // "Query  [F5 = chalao, F6 = samjhao]"

}  // namespace meradb::wb
```

  Panels read the session each frame (they own only view state: scroll offsets and caches). Border colours: Schema
  `kPurple`, Results `kCyan`, Log `kPink`, Query `kGreen` (tui.py's `$primary $accent $secondary $success`); focused: `kYellow`.

- [ ] **Step 1: Write `wb_screen_util.h`** (test helper, also used by Tasks 10, 11, 13):

```cpp
// cpp/tests/wb_screen_util.h
#pragma once
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>
#include <vector>

namespace wbtest {

// Renders an element into a width x height screen and returns the text of each row (a wide glyph's second cell is empty).
inline std::vector<std::string> renderLines(ftxui::Element e, int width, int height, ftxui::Screen* keep = nullptr) {
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, e);
    std::vector<std::string> lines;
    for (int y = 0; y < height; ++y) {
        std::string row;
        for (int x = 0; x < width; ++x) row += screen.PixelAt(x, y).character;
        while (!row.empty() && row.back() == ' ') row.pop_back();   // trailing blanks are noise
        lines.push_back(row);
    }
    if (keep) *keep = std::move(screen);
    return lines;
}
inline std::string fgOf(ftxui::Screen& s, int x, int y) { return s.PixelAt(x, y).foreground_color.Print(false); }
inline std::string bgOf(ftxui::Screen& s, int x, int y) { return s.PixelAt(x, y).background_color.Print(true); }
inline std::string rgbFg(int rgb) { return ftxui::Color::RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255).Print(false); }
inline std::string rgbBg(int rgb) { return ftxui::Color::RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255).Print(true); }
// Finds the first row containing `needle`; -1 if none. Useful to avoid hard-coding layout rows.
inline int findRow(const std::vector<std::string>& lines, const std::string& needle) {
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].find(needle) != std::string::npos) return static_cast<int>(i);
    return -1;
}

}  // namespace wbtest
```

  (`Screen::Create` returns a movable `Screen`; if the compiler complains about copying, return through the out-parameter only.)

- [ ] **Step 2: Failing tests** `test_wb_panels.cpp` (names `wbui`). Fixture: a `FakeBackend` session with `tree` set to the Task 5 sample
  JSON, a `ManualPoster`, `pumpIdle`, panels rendered with `panel->Render() | size(WIDTH, EQUAL, 40) | size(HEIGHT, EQUAL, 12)` via `renderLines`.
  Required cases:

  1. **Keys:** `keys::isRun(Event::F5)`, `isRun(keys::ctrl('R'))`, `isHistoryPrev(Event::ArrowUpCtrl)`, `isHistoryPrev(ctrl('P'))`,
     `ctrl('S').input() == "\x13"`, `ctrl('Q').input() == "\x11"`, `isQuit(ctrl('S'))` is false, `isExplain(Event::F5)` false.
  2. **lineToElement:** a `Line` with a pink bold `DIKHAO` then plain ` x` renders with the right characters, and the pixel foreground of
     `D` is `rgbFg(0xff79c6)` and bold; of `x` it is the default. *Parent-colour check*: wrap the element in the panel frame
     and confirm the inner text colour survives (if the frame's `color()` decorator overrides children, build the frame as
     `dbox({window(title, filler()) | color(accent), inset content})` instead; the test decides).
  3. **panelFrame:** the title appears on the top border row; the border cell colour is the accent colour, or `kYellow` when focused.
  4. **Tree panel:** rows `▼ Databases`, `  ▶ college`, `  ▼ main`, `    ▶ students`, `    ▶ marks` (expander markers `▼` open, `▶`
     closed, two spaces of indent per depth, leaves get two spaces instead of a marker); the selected row has background `kCurrentLine`
     when focused; `Event::ArrowDown` moves the selection; `Event::Return` on the `students` row expands it AND queues
     `DIKHAO * SE students SIRF 100;` (check `session.pendingJobs() == 1` and, after `pumpIdle`, the fake's `ranList()`); `Event::ArrowRight`
     on a collapsed table expands it, `ArrowLeft` collapses it; `Event::Character(" ")` toggles; a column row on Return inserts its name
     into the editor. Rows beyond the height scroll with the selection (use a tree with 30 tables).
  5. **Results panel:** with a 3-row result (ints, floats, text, NULL, bool): the title line contains `Results -- 3 row(s)`; the header row shows
     the column names; numbers are right-aligned in their column (compare the column of the last digit of `1` and `22`), `KHALI` is dim italic,
     `SACH` green and `JHOOTH` red (pixel foregrounds); odd data rows have the stripe background; `ArrowDown` / `PageDown` / `End` move the
     cursor row (background `kCurrentLine` while focused); `ArrowRight` shifts the view by 4 cells (text moves left by 4, never past the
     last column); a new result (`resultVersion` change) resets cursor and offset; no table yet -> an empty body and the title `Results`.
     With 200 rows only the visible rows are built (assert the render of a 10-row panel is fast: one render of 100,000 rows under 50 ms).
  6. **Log panel:** the start-up lines appear; an `Error` entry has foreground `rgbFg(kRed)`; `Message` green; a line longer than the width wraps
     by characters; new entries scroll the panel to the end while it is following; `ArrowUp` stops following, `End` resumes; `clearLog()`
     empties it; the wrapped-rows cache is not rebuilt for an unchanged log (render twice with a counter in a test-only hook, or just
     measure: 20,000 lines render in under 50 ms after the first frame).
  7. **Editor panel:** gutter ` 1 `, `DIKHAO` coloured pink and bold; the placeholder `Yahan query likho, jaise:  DIKHAO * SE students;   (F5 se chalao)`
     (dim) when empty; typing via `Event::Character("a")`, `Event::Return`, `Event::Backspace`, `Event::Delete`, arrows, `Home`, `End`
     edits the session's buffer; `Event::Character("é")` and a 4-byte emoji insert whole; shift-arrows (`Event::Special(keys::kShiftRight)`)
     select (background `kCurrentLine`); `keys::ctrl('A')` selects all; the cursor cell is inverted only while focused; with 12 lines in a
     5-row panel the view follows the cursor; a long line scrolls horizontally so the cursor stays visible.
  8. **Panels never throw** for a 1x1, 0-row or very wide render (`size(WIDTH, EQUAL, 1)`).

- [ ] **Step 3: Implement.** Rules:

  - `lineToElement(line)`: for each segment `Element e = text(seg.text)`; then in this order (named locals, one decorator at a time):
    `color(RGB)` if `fg >= 0`, `bgcolor` if `bg >= 0`, `bold`, `dim`, `italic`, `underlined`, `inverted`; collect with `push_back`, return `hbox`; an
    empty line is `text("")`. Never pass several stateful calls in one argument list.
  - `panelFrame`: `window(text(" " + title + " "), content)` then `| color(Color::RGB(...))` (accent, or yellow if focused) and `| bold` when focused. See the
    test in step 2.2 for the fallback layout if the decorator overrides child colours.
  - Every panel's `Render()` does: read its box from the previous frame (`reflect(box_)` on the returned element) to learn the inner size, update
    its `ScrollState`, build **only the visible rows**, and return `panelFrame(...) | reflect(box_)`. `innerHeight()` is `max(1, box_.y_max - box_.y_min + 1 - 2)`, width likewise.
    On the very first frame (empty box) assume 10 x 40.
  - **Tree:** row element = `hbox({text(indent), text(marker), lineToElement(clipLine(label, 0, room))})`; marker `"▼ "` / `"▶ "` for expandable rows, `"  "` otherwise;
    selected row gets `bgcolor(kCurrentLine)` if focused, `bgcolor(kStripe)` if not. Keys: Up/Down/PageUp/PageDown/Home/End move the selection
    (`tree.moveSelection`, `scroll()`), Right/Left `expandOrDescend` / `collapseOrAscend`, Return `session.activateTreeRow(selected)`, space `toggle(selected)`.
  - **Results:** build per-column strings with padding `" " + pad + " "` (numbers padded on the left); a data row is the concatenation, clipped by
    `clipLine(row, xOffset, innerWidth)`; header bold on `bgcolor(0x3b3e52)`; even/odd rows alternate background `kBackground` / `kStripe`; the cursor row
    uses `kCurrentLine` when focused. Left/Right change `xOffset_` by 4 within `[0, max(0, totalWidth - innerWidth)]`; Up/Down/PageUp/PageDown/Home/End the cursor
    (`ScrollState` height = innerHeight - 1). Wheel: scroll 3 rows.
  - **Log:** keep `wrapped_` (vector of `Line`) rebuilt from `session.log().entries()` when `generation()` or the width changed, or extended for new entries when only
    `revision()` grew; `ScrollState` over the wrapped rows; `follow_` true until the user scrolls up. Entries are rendered with their stored styles; an entry
    with several lines keeps them as separate rows. Up/Down/PageUp/PageDown scroll, Home top, End follow.
  - **Editor:** gutter `gutterText(row, n)` dim (the cursor row's number in `kText`), body `clipLine(editorRowLine(buffer, row, focused), left, bodyWidth)`, `top_` and `left_`
    adjusted each frame so the cursor row and `displayColumn(line, cursor.col)` are visible. Placeholder when `buffer.empty()`. `OnEvent` (only reached when this panel
    has focus and no global key matched): character -> `insert(event.character())`; `Return` -> `newline`; `Backspace`, `Delete`; `ArrowLeft/Right/Up/Down` and
    `Home/End/PageUp/PageDown` -> `move(..., false)`; the six `keys::kShift*` strings -> `move(..., true)`; `ArrowLeftCtrl/RightCtrl` -> word moves; `isSelectAll` -> `selectAll()`.
    Return `true` when handled. Do not handle `Tab` (the window cycles focus), F-keys or Ctrl keys here.

- [ ] **Step 4: Run `ctest -R wbui --output-on-failure`.** Expected: PASS. Fix layout surprises in the code, not in the expectations, unless a
  value was computed wrongly in this plan.

- [ ] **Step 5: Commit.**

```
git add cpp/include/meradb/wb_keys.h cpp/include/meradb/wb_panels.h cpp/src/wb_panels.cpp cpp/tests/wb_screen_util.h cpp/tests/test_wb_panels.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the workbench panels: schema tree, results table, log and query editor"
```

---

### Task 10: The window: layout, header, footer, key routing, focus

**Files:**
- Create: `cpp/include/meradb/wb_ui.h`, `cpp/src/wb_ui.cpp`
- Create: `cpp/tests/test_wb_window.cpp`
- Modify: the two `CMakeLists.txt`

**Interfaces:**

```cpp
// cpp/include/meradb/wb_ui.h
#pragma once
#include "meradb/wb_panels.h"
#include "meradb/wb_session.h"
#include <ftxui/component/component_base.hpp>

namespace meradb::wb {

class WorkbenchUi {
public:
    explicit WorkbenchUi(Session& session);
    ~WorkbenchUi();
    ftxui::Component component() const;     // the root: Render() draws everything, OnEvent() routes every key
    ftxui::Element render();                // component()->Render()
    bool onEvent(const ftxui::Event& e);    // component()->OnEvent(e)
    Panel focus() const;
    void setFocus(Panel panel);

    static constexpr int kMinWidth = 60;
    static constexpr int kMinHeight = 24;
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace meradb::wb
```

  Layout (tui.py CSS): vertical stack `header (1 row)`, `body (flex)`, `footer (1 row)`; body = horizontal `schema (width 32)` | `main (flex)`;
  main = vertical `results (flex)`, `log (height 10)`, `editor (height 9)`. If the terminal is smaller than 60 x 24 the window draws a single
  centred line `Terminal bahut chhota hai (kam se kam 60 x 24)` instead (keys still work).
  Header: ` MeraDB Workbench — <subtitle>` bold on a dark band (`0x3b3e52`), the busy label right-aligned (`[chal raha hai +1]`, yellow).
  Footer: bold key + description pairs separated by two spaces: `F5 Chalao`, `F6 Samjhao`, `^↑ Pichli`, `^↓ Agli`, `^S CSV`, `^O Connect`, `^L Log saaf`, `F1 Madad`, `^Q Bahar`.

  **Routing, in this order** (the whole table is the test list):

| Event | While no dialog is open | While a dialog is open |
|---|---|---|
| `keys::isQuit` (Ctrl+Q) | `session.requestQuit()` | the same |
| any event while `session.quitting()` | swallowed | swallowed |
| `isInterrupt` (Ctrl+C) | log a `Warn` entry with the exact text `Bahar niklne ke liye Ctrl+Q dabao.` | ignored |
| `isRun` | `session.runEditorText()` | ignored |
| `isExplain` | `session.explainEditorText()` | ignored |
| `isHistoryPrev` / `isHistoryNext` | `session.historyStep(-1 / +1)` | ignored |
| `isExport` | `session.exportCsv()` | ignored |
| `isConnect` | `session.openConnectDialog()` | ignored |
| `isClearLog` | `session.clearLog()` | ignored |
| `isHelp` | `session.showHelp()` | closes the help dialog |
| `Event::Tab` / `Event::TabReverse` | focus next / previous: Tree -> Results -> Log -> Editor -> Tree | handled by the dialog (Task 11) |
| mouse | Task 11 | Task 11 |
| anything else | the focused panel's `OnEvent` | the dialog's handler |

  After every event: `if (auto p = session.takeFocusRequest()) setFocus(*p)`. The initial focus is the Editor. Every frame calls
  `setFocused(panel == focus_)` on the four panels before rendering them.

- [ ] **Step 1: Failing tests** `test_wb_window.cpp` (names `wbui`, prefix `window`): fixture builds a real `Session` over `FakeBackend` plus the
  `WorkbenchUi`, `press(Event)`, `type("text")` (one `Event::Character` per code point), `pumpIdle`, `render(w, h)` via `renderLines`.
  Cases: (1) at 120 x 40 the first row contains `MeraDB Workbench — fake:1  |  db: main`; the last row contains `F5 Chalao`, `F6 Samjhao`, `^Q Bahar`;
  panel titles `Schema`, `Results`, `Log`, `Query  [F5 = chalao, F6 = samjhao]` are present; the schema column is 32 wide (the `│` of its right border is
  at x = 31); the log panel is 10 rows tall and the editor 9 (rows between their borders). (2) Typing `DIKHAO * SE t;` then `F5` runs it on the worker
  and the log shows `main> DIKHAO * SE t;` and `ok`; `Ctrl+R` does the same; the editor keeps its text. (3) `F6` on a single statement logs the
  `SAMJHAO` echo; on two statements the yellow warning. (4) Tab cycles Tree -> Results -> Log -> Editor -> Tree and Shift+Tab (`Event::TabReverse`) goes back;
  the focused panel's border is yellow, the others their accent; starting focus is the Editor. (5) `Ctrl+Up` / `Ctrl+Down` and `Ctrl+P` / `Ctrl+N` walk
  the history and put focus on the Editor even if the Tree had it. (6) `Ctrl+S` before any result logs the yellow hint; after a result it exports (use
  a temp `exportBaseDir`). (7) `Ctrl+L` clears the log. (8) `Ctrl+C` logs the hint and nothing quits. (9) `Ctrl+Q` with an open transaction
  (`SHURU;` ran) calls the exit callback after the pump and the fake saw `rolledBackOnClose`; while the first statement is blocked, the header shows
  `[band ho raha hai ...]` and keys are swallowed. (10) Sizes: 60 x 24 renders the full layout without throwing; 59 x 24 and 80 x 23 show the too-small message;
  200 x 60 and 1000 x 3 do not throw. (11) The busy label appears in the header while a statement is blocked and vanishes after. (12) Selection-aware
  run through the keyboard: type `one;`, Enter, `two;`, move to the start with `Event::Special(keys::kShiftUp)`... (use `Ctrl+A` to select all, then
  `ArrowLeft`, `Shift+Right` x4) -> `F5` runs only `one;`.

- [ ] **Step 2: Implement `WorkbenchUi`.** Compose with `hbox`, `vbox`, `size`, `flex`, `separator`-free borders (each panel has its own frame). Build the
  `Elements` vectors with `push_back` in screen order. Header/footer pieces are `lineToElement` of `Line`s built from `Style`s so tests can check colours.
  The root component class is a `ftxui::ComponentBase` subclass inside `Impl`; `Render()` must be callable with no `ScreenInteractive` (tests). The busy
  spinner is optional: if used, call `ScreenInteractive::Active()->RequestAnimationFrame()` only when `Active() != nullptr`.

- [ ] **Step 3: Run `ctest -R wbui`.** Expected: PASS.

- [ ] **Step 4: Commit.**

```
git add cpp/include/meradb/wb_ui.h cpp/src/wb_ui.cpp cpp/tests/test_wb_window.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the workbench window: layout, header, footer, key routing and focus cycling"
```

---

### Task 11: Modals: help and connect dialogs, busy indicator, mouse

**Files:**
- Create: `cpp/include/meradb/wb_form.h`, `cpp/src/wb_form.cpp` (pure `LineEdit` and `ConnectForm`)
- Create: `cpp/include/meradb/wb_dialogs.h`, `cpp/src/wb_dialogs.cpp` (FTXUI rendering of the two dialogs)
- Modify: `cpp/src/wb_ui.cpp` (draw the open dialog on top, route events to it, mouse)
- Create: `cpp/tests/test_wb_form.cpp`, `cpp/tests/test_wb_dialogs.cpp`
- Modify: the two `CMakeLists.txt`

**Interfaces:**

```cpp
// cpp/include/meradb/wb_form.h
#pragma once
#include "meradb/wb_session.h"
#include <string>

namespace meradb::wb {

// A one-line text field: code-point cursor, insert, Backspace, Delete, Left/Right/Home/End. `password` shows '*' per character.
class LineEdit {
public:
    explicit LineEdit(std::string text = "", bool password = false) : text_(std::move(text)), password_(password) { cursor_ = length(); }
    const std::string& text() const { return text_; }
    std::string shown() const;               // text, or one '*' per code point
    int cursor() const { return cursor_; }   // code points
    int length() const;
    void insert(const std::string& utf8);    // control characters ignored
    void backspace();
    void del();
    void left();
    void right();
    void home();
    void end();
    void setText(const std::string& text);   // cursor to the end
    bool digitsOnly = false;                 // the Port field: only 0-9 are accepted (Textual: type="integer")
private:
    std::string text_;
    bool password_;
    int cursor_ = 0;
};

enum class FormAction { None, Connect, Local, Cancel };

// tui.py ConnectScreen: Host, Port, Password, Database, then the buttons Connect / Local mode / Cancel.
class ConnectForm {
public:
    static constexpr int kFields = 4;
    static constexpr int kButtons = 3;
    ConnectForm(const std::string& host, const std::string& port);   // password hidden; port digits only
    LineEdit& field(int i) { return fields_[static_cast<std::size_t>(i)]; }
    const LineEdit& field(int i) const { return fields_[static_cast<std::size_t>(i)]; }
    int active() const { return active_; }   // 0..3 fields, 4..6 buttons
    void next();                             // Tab / Down: wraps
    void previous();                         // Shift+Tab / Up: wraps
    void setActive(int index);
    // Enter: on a field = Connect (Textual: Input.Submitted); on a button = that button. Escape = Cancel.
    FormAction enter() const;
    FormAction escape() const { return FormAction::Cancel; }
    ConnectRequest request(bool local) const;   // makeConnectRequest(local, host, port, password, database)
private:
    std::array<LineEdit, 4> fields_;
    int active_ = 0;
};

}  // namespace meradb::wb
```

  (add `#include <array>`.) The dialogs header declares `ftxui::Element renderConnectDialog(const ConnectForm&)`, `class HelpDialog { ... }` (see below) and
  `ftxui::Element modalOverlay(ftxui::Element base, ftxui::Element dialog)`.

  **Connect dialog** (tui.py): a centred box 64 wide (or the terminal width - 2), title `MeraDB server se connect karo` (bold), labels `Host`, `Port`,
  `Password (agar server par hai)`, `Database (optional)` above their fields, buttons `[ Connect ]  [ Local mode ]  [ Cancel ]`; the active field shows the
  cursor as an inverse cell; the active button is inverted/bold. Keys: characters / Backspace / Delete / Left / Right / Home / End edit the active field;
  `Tab`, `ArrowDown` next, `TabReverse`, `ArrowUp` previous; `Return` -> `enter()`; `Escape` -> cancel. `FormAction::Connect` calls
  `session.connect(form.request(false))`, `Local` calls `session.connect(form.request(true))`, `Cancel` calls `session.closeModal()`; connect and local also close the
  dialog (the session does it).

  **Help dialog:** a box 90 % of the width and height, title ` Madad `; content is `renderHelp(innerWidth)` cached per width; Up/Down scroll one row, PageUp/PageDown a page,
  Home/End the ends; `Escape`, `F1` and the character `q` close it (`session.closeModal()`).

- [ ] **Step 1: Failing tests.** `test_wb_form.cpp` (names `wbform`): `LineEdit` typing `héllo`, Left x2, Backspace -> `hélo`; `digitsOnly` ignores letters and a pasted `12ab3` becomes `123`;
  `shown()` of a password field is `*****` for `héllo`; Home/End/Delete; cursor never leaves `[0, length]`. `ConnectForm("127.0.0.1", "6372")`: initial active 0; `next()` x7
  wraps to 0; `previous()` from 0 goes to 6; `enter()` is `Connect` on any field, and `Connect`/`Local`/`Cancel` on buttons 4/5/6; `request(false)` after typing a password
  `pw` and database ` db ` gives host `127.0.0.1`, port `6372`, password `pw`, database `db`; emptying the host gives the default host; `request(true).local`.
  `test_wb_dialogs.cpp` (names `wbui`, prefix `dialog`), with the session/window fixture of Task 10: (1) `Ctrl+O` opens the dialog: the screen shows
  `MeraDB server se connect karo`, `Host`, `127.0.0.1`, `6372`, `Password (agar server par hai)`, `Database (optional)`, `Connect`, `Local mode`, `Cancel`; while it is open
  `F5` / `Ctrl+S` do nothing and `Ctrl+Q` still quits; (2) typing in the host field changes it; the password field shows `*`; `Tab` moves; `Esc` closes without calling the
  factory; (3) `Return` with a fake factory records the request (host, port, password, database) and the dialog closes, the busy label shows, then the header changes to the
  new backend's description; (4) the Local button passes `local == true`; (5) a failing factory logs `Connect nahi hua: ...` and keeps the old backend; (6) `F1` opens the help:
  the screen shows `MeraDB Workbench: Madad`, `Ctrl+Q`, `Bahar niklo`; `PageDown` scrolls (a later heading such as `Data types` or `Transactions` eventually appears); `Esc`, `F1` and
  `q` each close it; (7) the dialogs render at 60 x 24 and 200 x 60 without throwing; (8) mouse: a left click inside the Results panel's box focuses it, inside the Tree
  the tree, wheel up/down over the Log scrolls it (`scrollWheel`); events outside every panel are ignored; with a dialog open the mouse is ignored.

- [ ] **Step 2: Implement.** `LineEdit` works on code points (same helpers as `TextBuffer`: put the shared UTF-8 helpers in an anonymous namespace of `wb_form.cpp`
  or declare them once in `wb_editor.h` as `int cpCount(const std::string&)` / `std::size_t byteAt(const std::string&, int)` and use them from both; prefer sharing).
  `ConnectForm` fields: host (`DEFAULT` given), port (`digitsOnly = true`), password (`password = true`), database. The overlay: `dbox({base, clear_under(center(dialog))})`.
  In `wb_ui.cpp`: when `session.modal() != Modal::None`, `render()` returns the overlay; `onEvent` routes to the dialog handler as in Task 10's table; the `ConnectForm` is created
  when the modal opens (`session.connectDefaults()`), destroyed when it closes; the `HelpDialog` keeps its `ScrollState`. Mouse (`event.is_mouse()`, left press inside a panel's `box()`
  -> `setFocus`; `Mouse::WheelUp/WheelDown` over a panel -> `panel->scrollWheel(-1 / +1)`); the tree additionally selects the clicked row. Keep this part small; if it grows beyond
  about 60 lines, ship it without the tree click and say so in Task 14's divergences.

- [ ] **Step 3: Run `ctest -R "wbform|wbui"`.** Expected: PASS. Also run a manual render: temporarily write a tiny `main` (or use `wb_keyprobe`'s pattern) is NOT needed;
  Task 12 runs the real program.

- [ ] **Step 4: Commit.**

```
git add cpp/include/meradb/wb_form.h cpp/src/wb_form.cpp cpp/include/meradb/wb_dialogs.h cpp/src/wb_dialogs.cpp cpp/src/wb_ui.cpp cpp/tests/test_wb_form.cpp cpp/tests/test_wb_dialogs.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the connect and help dialogs, the busy label and basic mouse support to the workbench"
```

---

### Task 12: Terminal-mode guard, `runWorkbench`, CLI wiring

**Files:**
- Modify: `cpp/include/meradb/sys_compat.h`, `cpp/src/sys_compat.cpp` (`TerminalModeGuard`)
- Modify: `cpp/tests/test_sys_compat.cpp` (one no-terminal test)
- Modify: `cpp/src/workbench.cpp` (the real `runWorkbench` and `UiBridge`)
- Modify: `cpp/include/meradb/cli.h`, `cpp/src/cli.cpp`, `cpp/src/main.cpp`, `cpp/tests/test_cli.cpp`
- Create: `cpp/tests/wb_cli_smoke.py`
- Modify: `cpp/tests/CMakeLists.txt` (register `wb_cli_smoke`), `cpp/tests/tools/wb_keyprobe.cpp` (`--guard`)

**Interfaces:**

```cpp
// sys_compat.h -- next to AnsiConsole / InterruptGuard
// While alive, the terminal's own key handling is switched off so the full-screen workbench receives these keys as
// ordinary input: POSIX ISIG (Ctrl+C, Ctrl+Z, Ctrl+\), IXON (Ctrl+S / Ctrl+Q flow control) and IEXTEN (Ctrl+V, Ctrl+O);
// Windows ENABLE_PROCESSED_INPUT (Ctrl+C). Does nothing when stdin is not a terminal. The destructor puts the old
// mode back. Create it BEFORE the screen's Loop(): FTXUI saves the terminal state when the loop starts and restores
// it when the loop ends, so ours is the outer layer and is restored last.
class TerminalModeGuard {
public:
    TerminalModeGuard();
    ~TerminalModeGuard();
    TerminalModeGuard(const TerminalModeGuard&) = delete;
    TerminalModeGuard& operator=(const TerminalModeGuard&) = delete;
    bool active() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
```

```cpp
// cli.h additions
// The workbench lives in a library that depends on this one, so cliMain reaches it through a hook that main() sets.
using WorkbenchRunner = int (*)(std::unique_ptr<Backend> backend, const CliArgs& args);
void setWorkbenchRunner(WorkbenchRunner runner);   // nullptr (the default): the workbench is not built in

// A test seam like ShellInputOverride: while alive, `workbench` believes stdin/stdout are (or are not) terminals.
class WorkbenchTerminalOverride {
public:
    explicit WorkbenchTerminalOverride(bool isTerminal);
    ~WorkbenchTerminalOverride();
    WorkbenchTerminalOverride(const WorkbenchTerminalOverride&) = delete;
    WorkbenchTerminalOverride& operator=(const WorkbenchTerminalOverride&) = delete;
};
```

- [ ] **Step 1: Failing tests.**
  - `test_sys_compat.cpp`: `TEST_CASE("sys TerminalModeGuard is inert without a terminal")`: construct and destroy one; if `!sys::isTerminal(0)`, `active()` is false. (When a developer runs the
    tests from a terminal it will be active and must restore the mode; the test only checks it does not crash.)
  - `test_cli.cpp`, new cases (name prefix `cli workbench`), using a static fake runner that records its arguments and returns 7:
    1. no runner set + `cliMain({"workbench"})` -> 1 and stderr contains `abhi C++ version mein nahi hai` (the existing test, kept unchanged);
    2. runner set + `WorkbenchTerminalOverride(false)` -> returns 1, stderr contains `terminal chahiye`, the runner was NOT called, and no backend was opened (use a bad `-D` path that would fail if
       opened: no error about it);
    3. runner set + override(true) + `{"workbench", "--local", "-D", <tmp>}` -> the runner is called once with a backend whose `description()` starts with `local (`, `args.dataDir == tmp`,
       `args.command == "workbench"`, and `cliMain` returns the runner's value (7); the runner closes the backend itself (call `backend->close()` in the fake);
    4. `{"tui", ...}` and `{"--tui", ...}` do the same; `-d college` selects the database (`description` unchanged, `currentDb() == "college"` after `BANAO DATABASE college;` pre-created
       in the temp folder);
    5. a bad option (`workbench --nope`) still exits 2 with argparse's text and calls nothing; `workbench -h` prints the usage and exits 0 without calling the runner;
    6. an unreachable explicit server (start a `RunningServer`, remember its port, stop it, then `-H 127.0.0.1 -p <that port>`) -> exit 1, the error on stderr, runner not called;
    7. a runner that throws `std::runtime_error("boom")` -> `cliMain` returns 1 with `boom` on stderr.
    Restore the runner to `nullptr` at the end of each case (a small RAII helper).
  - `wb_cli_smoke.py` (Python, registered when Python exists): runs the real binary with `stdin=DEVNULL`: `workbench`, `tui`, `--tui` each exit 1 and print, on stderr, either
    `terminal chahiye` (workbench built in) or `abhi C++ version mein nahi hai` (built with `MERADB_WORKBENCH=OFF`); `workbench --bogus` exits 2; `workbench -h` exits 0 and starts with
    `usage: meradb workbench`. Environment as in the other scripts (no `MERADB_*`, temp folders).

- [ ] **Step 2: Implement `TerminalModeGuard`** in `sys_compat.cpp` (the only file with `<windows.h>` / `<termios.h>`; match the file's existing `#ifdef _WIN32` structure):

```cpp
struct TerminalModeGuard::State {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD mode = 0;
#else
    struct termios saved;
#endif
};

TerminalModeGuard::TerminalModeGuard() {
    if (!isTerminal(0)) return;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return;
    if (!SetConsoleMode(h, mode & ~static_cast<DWORD>(ENABLE_PROCESSED_INPUT))) return;
    state_.reset(new State);
    state_->handle = h;
    state_->mode = mode;
#else
    struct termios t;
    if (tcgetattr(0, &t) != 0) return;
    const struct termios saved = t;
    t.c_lflag &= ~static_cast<tcflag_t>(ISIG | IEXTEN);
    t.c_iflag &= ~static_cast<tcflag_t>(IXON);
    if (tcsetattr(0, TCSANOW, &t) != 0) return;
    state_.reset(new State);
    state_->saved = saved;
#endif
}

TerminalModeGuard::~TerminalModeGuard() {
    if (!state_) return;
#ifdef _WIN32
    SetConsoleMode(state_->handle, state_->mode);
#else
    tcsetattr(0, TCSANOW, &state_->saved);
#endif
}

bool TerminalModeGuard::active() const { return state_ != nullptr; }
```

  (Add `#include <termios.h>` in the POSIX branch.) Extend `wb_keyprobe` with a `--guard` argument that creates a `TerminalModeGuard` before `Loop`, then **run it by hand** on each terminal you have:
  with `--guard`, Ctrl+C, Ctrl+S, Ctrl+Q, Ctrl+O, Ctrl+R must show up as bytes `03 13 11 0F 12` and must not exit or freeze the probe. Without it, note what happens (Task 1 step 7). If FTXUI
  overwrites the mode when its loop starts (Ctrl+C still ends the probe with `--guard`), change the workbench to apply the guard from the first task posted to the screen
  (`screen.Post([&]{ guard.apply(); })`; give `TerminalModeGuard` a deferred `apply()` and make its destructor restore only what `apply()` changed) and say so in Task 14.

- [ ] **Step 3: Implement `runWorkbench`** (`workbench.cpp`):

```cpp
// cpp/src/workbench.cpp
#include "meradb/workbench.h"
#include "meradb/sys_compat.h"
#include "meradb/wb_session.h"
#include "meradb/wb_ui.h"
#include <ftxui/component/screen_interactive.hpp>
#include <iostream>
#include <mutex>

namespace meradb::wb {

namespace {

// Hands closures from the worker to the UI thread, and stops once the loop has ended (a closure posted after
// Loop() returns would be dropped by FTXUI anyway; this makes it explicit and race-free).
class UiBridge {
public:
    explicit UiBridge(ftxui::ScreenInteractive* screen) : screen_(screen) {}
    void post(std::function<void()> f) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) screen_->Post(std::move(f));
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = false;
    }

private:
    std::mutex mutex_;
    ftxui::ScreenInteractive* screen_;
    bool open_ = true;
};

}  // namespace

int runWorkbench(std::unique_ptr<Backend> backend, const CliArgs& args) {
    ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
    UiBridge bridge(&screen);
    SessionOptions options;
    options.dataDir = args.dataDir;
    Session session(std::move(backend), options, [&bridge](std::function<void()> f) { bridge.post(std::move(f)); });
    session.setOnExit([&screen] { screen.Exit(); });
    WorkbenchUi ui(session);
    {
        sys::TerminalModeGuard guard;  // before Loop (see its header comment)
        screen.Loop(ui.component());
    }
    bridge.close();
    if (session.busy())
        std::cerr << "Chalta hua statement khatam hone ka intezaar hai, phir transaction WAPAS hoga ...\n";
    session.shutdown();  // waits for the worker, closes the backend (rolls an open transaction back)
    return 0;
}

}  // namespace meradb::wb
```

  Order of destruction matters and is the reason for the explicit calls: `bridge.close()` first (no more UI closures), then `session.shutdown()` (joins the worker), and only
  then do `ui`, `session`, `bridge`, `screen` go out of scope (declared in that order, so destroyed in the reverse order).

- [ ] **Step 4: CLI wiring.** `cli.cpp`: a file-local `WorkbenchRunner g_workbenchRunner = nullptr;` and `std::optional<bool> g_terminalOverride;`; `setWorkbenchRunner`,
  `WorkbenchTerminalOverride`; replace the final "workbench" block of `cliMain` (the one that prints the note) with `return runWorkbenchCommand(args);`:

```cpp
int runWorkbenchCommand(const CliArgs& args) {
    if (g_workbenchRunner == nullptr) {
        note("`meradb workbench` abhi C++ version mein nahi hai (is build mein workbench shaamil nahi). "
             "Python version istemal karo, ya scripts ke liye:  meradb run FILE");
        return 1;
    }
    const bool terminal = g_terminalOverride ? *g_terminalOverride : (sys::isTerminal(0) && sys::isTerminal(1));
    if (!terminal) {
        note("Workbench ke liye terminal chahiye (stdin aur stdout dono terminal hone chahiye). "
             "Scripts ke liye:  meradb run FILE");
        return 1;
    }
    std::unique_ptr<Backend> backend = openBackend(args);   // notes on stderr, may throw MeraDBError (exit 1)
    return g_workbenchRunner(std::move(backend), args);
}
```

  (`test_cli.cpp` case 1 above asserts the substring `abhi C++ version mein nahi hai`, which both the old and the new text contain.) `main.cpp`:

```cpp
#include "meradb/cli.h"
#include "meradb/sys_compat.h"
#ifdef MERADB_HAVE_WORKBENCH
#include "meradb/workbench.h"
#endif

int main(int argc, char** argv) {
    meradb::sys::Utf8Console console;  // UTF-8 on a Windows console; untouched when piped
#ifdef MERADB_HAVE_WORKBENCH
    meradb::setWorkbenchRunner(&meradb::wb::runWorkbench);
#endif
    return meradb::cliMain(meradb::sys::commandLineArgs(argc, argv));
}
```

  The Windows console co-exists with FTXUI as follows: `Utf8Console` (code page 65001 for output and input) stays alive for the whole run, FTXUI enables virtual-terminal
  output/input and restores the console modes itself when its loop ends, and `TerminalModeGuard` clears and restores only `ENABLE_PROCESSED_INPUT`. Neither `sys::AnsiConsole` nor
  `sys::InterruptGuard` / `ConsoleLineSource` may be used inside the workbench (header comment in `workbench.cpp`).

- [ ] **Step 5: Build with and without the option; run `ctest`.** Expected: all green in both configurations (`-DMERADB_WORKBENCH=OFF` in a separate build directory:
  `cli workbench` case 1 and `wb_cli_smoke` accept the stub note; the `meradb_wb_tests` target does not exist there).

- [ ] **Step 6: Try the real program once by hand** in a terminal: `cpp/build/meradb_cli workbench --local -D <empty temp folder>`. Expected: the full screen appears, `Namaste! Connected: local (...)` in the log, F1 opens the
  help, `BANAO TABLE t (id ANK);` + F5 runs, Ctrl+Q leaves the terminal in its normal state, exit code 0. (The complete manual checklist is Task 14.)

- [ ] **Step 7: Commit.**

```
git add cpp/include/meradb/sys_compat.h cpp/src/sys_compat.cpp cpp/tests/test_sys_compat.cpp cpp/src/workbench.cpp cpp/include/meradb/cli.h cpp/src/cli.cpp cpp/src/main.cpp cpp/tests/test_cli.cpp cpp/tests/wb_cli_smoke.py cpp/tests/CMakeLists.txt cpp/tests/tools/wb_keyprobe.cpp
git commit -m "Wire the workbench into the command line with a terminal-mode guard and a worker-thread session"
```

# BATCH D — end-to-end checks, documentation, the manual checklist

### Task 13: End to end: the headless UI test and the Python Pilot comparison

**Files:**
- Create: `cpp/tests/test_wb_e2e.cpp` (the real `LocalBackend`, the real `Session`, the real component tree, synthetic events)
- Create: `cpp/tests/wb_probe.cpp` (runs one scenario against the C++ `Session`, prints observables as JSON)
- Create: `cpp/tests/workbench_scenarios.json`, `cpp/tests/workbench_pilot.py`, `cpp/tests/workbench_diff.py`
- Modify: `cpp/tests/CMakeLists.txt` (`test_wb_e2e.cpp` into `meradb_wb_tests`; `wb_probe` target; the `workbench_diff` test, only when Python is found and `TARGET meradb_workbench`)

**Part A — the in-process end-to-end test** (`test_wb_e2e.cpp`, names `wbe2e`). A fixture owns a `TempDir` for data and one for the
export base, a `ManualPoster`, a `LocalBackend` on the data folder, a `Session` and a `WorkbenchUi`, and offers `press(Event)`, `type(text)` (one `Event::Character`
per code point; `"\n"` becomes `Event::Return`), `settle()` (`pumpIdle`), `screen(w, h)` (lines via `renderLines`). All of it runs on the test thread plus the session's worker.
It never touches the real data folder. Scenarios (each is one `TEST_CASE`; screen assertions use `findRow` and substrings, not absolute rows):

1. **Start:** header text, `Namaste! Connected: local (` + the temp folder, `F1 dabao madad ke liye.`, tree `Databases` / `main`, focus on the editor (cursor cell inverted).
2. **DDL, DML, SELECT through the keyboard:** type `BANAO TABLE students (id ANK MUKHYA KUNJI, naam TEXT ZAROORI, marks DASHAMLAV, pass BOOLEAN);`, `F5`; the log shows
   the echo with highlighted keywords (foreground `0xff79c6` on `BANAO`) and the green message; the tree gains `students` (collapsed). Replace the editor text
   (`Ctrl+A`, type) with an INSERT of three rows including a NULL and a false, run, then `DIKHAO * SE students;`, run: the results panel shows header `id naam marks pass`,
   `KHALI` dim italic, `SACH` green, `JHOOTH` red, numbers right-aligned, title `Results -- 3 row(s)`, a `(x.x ms)` dim line in the log. (If Python rejects a statement
   spelling when you write the matching Pilot scenario, fix the statement: the oracle decides.)
3. **Errors and several statements:** `DIKHAO * SE nahi_hai; DIKHAO 1 + 1;` -> a red error line, then the table of the second statement; the table of the LAST
   result stays after a later statement that returns no table.
4. **Selection-aware run and explain:** two statements in the editor, select the first with `Home`, `Shift+End` (as `Event::Special(keys::kShiftEnd)`) -> `F5` runs only it; `F6`
   explains a single statement (the log echo starts `main> SAMJHAO `) and warns for two.
5. **History:** `Ctrl+Up` x2 / `Ctrl+Down` x2 / past the end, as in `tui.py`, with focus returning to the editor; duplicates collapse.
6. **Tree:** focus the tree (`Tab`), expand `students` with `ArrowRight`, move to a column, `Return` inserts its name at the editor cursor and focuses the editor; `Return` on the
   table row runs `DIKHAO * SE students SIRF 100;`; create a second database (`BANAO DATABASE college;`), `Return` on it runs `ISTEMAL college;` and the header says `db: college`;
   expansion state survives each refresh.
7. **Transaction indicator:** `SHURU;` -> header contains `TRANSACTION (PAKKA / WAPAS)`; an INSERT inside; `WAPAS;` -> indicator gone and the row is gone.
8. **Quit rolls back:** `SHURU;`, an INSERT, `Ctrl+Q` -> after the pump the exit callback fired; a NEW `LocalBackend` on the same folder sees no inserted row and the folder has no
   `.wapas` leftovers. While a deliberately slow statement runs (a cross join of two 1,500-row tables) `Ctrl+Q` waits for it, then exits; `Ctrl+C` logs the hint and nothing else happens.
9. **CSV export:** run a SELECT with a comma, a quote and a NULL in the data; `Ctrl+S`; the file under `<exportBase>/exports/` equals Python's `csv.writer` output byte for byte
   (hard-code the expected bytes with `\r\n`), and the log's last line is `3 row(s) CSV mein save: <path>`.
10. **Connect dialog against a real server:** a `RunningServer` (from `server_fixture.h`, port chosen by the OS); `Ctrl+O`, clear the Port field (`Home`, then `Delete` x4 or `Backspace` x4), type the
    server's port, `Return`; after the pump the header reads `127.0.0.1:<port>  |  db: main`, a bold green `Connected: 127.0.0.1:<port>` line is in the log, the old local backend is closed, statements
    now go over the wire; `Ctrl+O` again (the Host field is active), `Tab` x5 reaches the `Local mode` button, `Return` -> back to `local (`. A wrong port logs
    `Connect nahi hua: [Connection Galti] ...` in bold red and keeps the old connection. Stop the server before the end of the test.
11. **Help:** `F1` shows the key table and, after `PageDown`, a later section of the language reference; `Esc` closes it; while it is open `F5` does nothing.
12. **Resize and Unicode:** render at 60 x 24, 80 x 24, 120 x 40, 200 x 60 (no exceptions, header always present); too small shows the message; type `'é😀नमस्ते日本'` into a string literal,
    run an INSERT and a SELECT: the table cells show the text intact and column alignment accounts for the double-width characters (the right border of the results panel stays in one column).
13. **Local to local:** `Ctrl+O` -> Local mode while a transaction is open on the current local engine: the transaction is rolled back (the old engine is closed first) and the new session starts clean.

Run it 20 times (`--repeat until-fail:20`): no flakiness, no leftover temp folders, and no thread still running after the fixture is destroyed.

**Part B — the comparison with the Python workbench.** Textual 8.x and its `Pilot` are installed on the dev machine (`python -c "import textual; print(textual.__version__)"` printed
`8.2.8` when this plan was written). The comparison drives *both* implementations through the same scenario file and compares what each shows, as JSON:

`cpp/tests/workbench_scenarios.json`: a list of `{ "name": ..., "steps": [...] }`. Step forms: `{"set_text": "..."}`, `{"select": [r1, c1, r2, c2]}` (anchor, cursor; rows 0-based, columns in
characters), `{"run": true}` (F5), `{"explain": true}` (F6), `{"history": -1}`, `{"export": true}`, `{"tree_enter": ["Databases", "main", "students"]}` (labels of the visible nodes, plain text;
Enter on that node), `{"clear_log": true}`, `{"connect": "local"}` (the dialog's Local mode), `{"snapshot": "label"}`. Scenarios (write them out in full; every statement must be accepted by
Python):

| Name | What it covers |
|---|---|
| `startup` | snapshot right after start-up |
| `table_roundtrip` | `BANAO TABLE` (PK, NOT NULL, UNIQUE, float, boolean columns), `DAALO` three rows incl. KHALI / SACH / JHOOTH, `DIKHAO * SE`, snapshots after each |
| `errors_and_multi` | an unknown table, two statements in one run, a syntax error, an empty-result SELECT (columns, zero rows) followed by a message-only statement |
| `history_explain` | three runs, `history -1` x2, `history 1` x3, F6 on one statement / two statements / empty text / bad syntax |
| `selection` | two statements, `select` the second, run; `select` a sub-range, explain |
| `tree` | two databases, tables with all column flavours (`id int PK NN`, `email text UQ`, ...), `tree_enter` on a table (expansion + SELECT), on a column (inserts the name), on the other database (`ISTEMAL`), snapshots |
| `transaction` | `SHURU;`, an INSERT, snapshot (indicator), `WAPAS;`, snapshot |
| `export` | `export` before any result, a SELECT with `,` `"` and NULL, `export` again |
| `clear_log` | runs, `clear_log`, one more run |
| `unicode` | a table with `é😀नमस्ते` and Chinese text |
| `connect_local` | `connect` local, then a statement |

Observables (`snapshot` records them under its label): `subtitle`, `editor` (text), `history` (list), `results_title`, `columns`, `rows` (`[[{t, k}]]` with `k` one of `null true false number plain`),
`log` (`[{k, t}]`, `k` one of `plain bold dim error message connected warn echo`; an entry's text joins its lines with `\n`), `tree` (`[{depth, label, expanded}]` for EVERY node, collapsed or not,
depth first, root first), and `csv` (the text of the newest file in `./exports/`, read with `newline=""`, or absent).

`workbench_pilot.py` drives `meradb/tui.py`:

```python
#!/usr/bin/env python3
"""Runs one scenario of workbench_scenarios.json against meradb/tui.py with Textual's Pilot; prints observables as JSON."""
import argparse
import asyncio
import glob
import json
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

LOG_CLASSES = {"bold red": "error", "#50fa7b": "message", "bold #50fa7b": "connected", "dim": "dim",
               "yellow": "warn", "bold": "bold"}
CELL_CLASSES = {"dim italic": "null", "green": "true", "red": "false", "#bd93f9": "number", "": "plain"}


def log_class(text):
    kind = LOG_CLASSES.get(str(text.style), "plain")
    if kind == "dim" and re.match(r"^\S+> ", text.plain):  # Text("<db>> ", dim) + highlighted(...) keeps the dim base style
        kind = "echo"
    return kind


def label_of(node):
    return node.label.plain if hasattr(node.label, "plain") else str(node.label)


def walk(node, depth=0):
    yield {"depth": depth, "label": label_of(node), "expanded": bool(node.is_expanded)}
    for child in node.children:
        yield from walk(child, depth + 1)


def find_node(tree, path):
    node = tree.root
    assert label_of(node) == path[0], path
    for label in path[1:]:
        node = next(c for c in node.children if label_of(c) == label)
    return node


def observe(app, logged):
    from meradb.tui import QueryEditor
    from textual.widgets import DataTable, Tree

    table = app.query_one("#results", DataTable)
    out = {
        "subtitle": app.sub_title,
        "editor": app.query_one(QueryEditor).text,
        "history": list(app.history),
        "results_title": table.border_title,
        "columns": [label_of(c) if hasattr(c.label, "plain") else str(c.label) for c in table.columns.values()],
        "rows": [[{"t": c.plain, "k": CELL_CLASSES.get(str(c.style), "plain")} for c in table.get_row_at(i)]
                 for i in range(table.row_count)],
        "log": [{"k": log_class(t), "t": t.plain} for t in logged],
        "tree": list(walk(app.query_one("#schema", Tree).root)),
    }
    files = glob.glob(os.path.join("exports", "*.csv"))
    if files:
        with open(max(files, key=os.path.getmtime), newline="", encoding="utf-8") as f:
            out["csv"] = f.read()
    return out


async def run(scenario, data_dir):
    from meradb.engine import Engine
    from meradb.tui import MeraDBApp, QueryEditor
    from textual.widgets import Tree
    from textual.widgets.text_area import Selection

    logged = []
    app = MeraDBApp(Engine(data_dir), data_dir=data_dir)
    original = app.log_line
    app.log_line = lambda text: (logged.append(text), original(text))[1]
    clear = app.action_clear_log
    app.action_clear_log = lambda: (logged.clear(), clear())[1]
    snaps = {}
    async with app.run_test(size=(220, 60)) as pilot:
        await pilot.pause()
        for step in scenario["steps"]:
            editor = app.query_one(QueryEditor)
            if "set_text" in step:
                editor.text = step["set_text"]
            elif "select" in step:
                r1, c1, r2, c2 = step["select"]
                editor.selection = Selection(start=(r1, c1), end=(r2, c2))
            elif "run" in step:
                app.action_run_query()
            elif "explain" in step:
                app.action_explain()
            elif "history" in step:
                app.action_history(step["history"])
            elif "export" in step:
                app.action_export_csv()
            elif "tree_enter" in step:
                tree = app.query_one("#schema", Tree)
                tree.select_node(find_node(tree, step["tree_enter"]))
            elif "clear_log" in step:
                app.action_clear_log()
            elif "connect" in step:
                app._switch_backend({"mode": "local", "host": "127.0.0.1", "port": "6372", "password": None, "database": None})
            elif "snapshot" in step:
                snaps[step["snapshot"]] = observe(app, logged)
            await pilot.pause()
    app.backend.close()
    return snaps


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenarios")
    parser.add_argument("name")
    parser.add_argument("--data", required=True)
    parser.add_argument("--cwd", required=True)
    args = parser.parse_args()
    scenario = next(s for s in json.load(open(args.scenarios, encoding="utf-8")) if s["name"] == args.name)
    os.chdir(args.cwd)
    print(json.dumps(asyncio.run(run(scenario, args.data)), ensure_ascii=False))


if __name__ == "__main__":
    main()
```

  (Adjust attribute names against the installed Textual if a call fails, for example how a `DataTable` column label is read; the contract is the JSON.)

`wb_probe.cpp` does the same with the C++ session: `LocalBackend(dataDir)`, a `ManualPoster`, `SessionOptions{dataDir, cwd, ...}`, each step calls the matching `Session` method then
`pumpIdle`; `tree_enter` finds the visible row whose chain of labels (tracked by depth while walking `tree().rows()`) equals the path and calls `activateTreeRow`; `snapshot` builds the JSON above
(`logKindName`, entries' lines joined with `\n`, `tree().allNodes()`, `table()`, `history().items()`, the newest `exports/*.csv` read in binary). Output with `nlohmann::json::dump()` (no pretty printing,
`ensure_ascii` off is the default).

`workbench_diff.py [--probe PATH]`: exits 77 (ctest `SKIP_RETURN_CODE`) when `import textual` fails; otherwise for every scenario creates two fresh data folders and two fresh working folders, runs the
pilot and the probe (child environment without `MERADB_*`, a 120 s timeout each), parses both outputs, **normalises** both (the data folder string -> `<DATA>`, the working folder -> `<CWD>`, regex
`\(\d+\.\d ms\)` -> `(<ms>)`, `CSV mein save: .*` -> `CSV mein save: <EXPORT>`, compared recursively on every string), and compares snapshot by snapshot, printing a `difflib.unified_diff` of the
pretty-printed JSON for each mismatch. Exit 0 when all scenarios match, 1 otherwise. `CMakeLists`: `add_test(NAME workbench_diff COMMAND ${Python3_EXECUTABLE} .../workbench_diff.py --probe $<TARGET_FILE:wb_probe>)`,
`TIMEOUT 600`, `SKIP_RETURN_CODE 77`.

**Known, accepted differences** the comparison must not trip over (keep the scenarios clear of them): a newline inside a cell (`↵`, R16), Unicode digits / word boundaries in highlighting (not observed here), wrapped
log lines (the observables are logical lines), the cursor position after a tree refresh (R14, not observed). Anything else that differs is a bug in the C++ workbench or in this plan; fix the code, or fix the plan
if Python is right.

- [ ] **Step 1: Write Part A and run it** (`ctest -R wbe2e --output-on-failure`, then `--repeat until-fail:20`). Expected: PASS.
- [ ] **Step 2: Write the scenario file, `workbench_pilot.py`, `wb_probe.cpp`, `workbench_diff.py`; run `python cpp/tests/workbench_diff.py --probe cpp/build/.../wb_probe`.** Expected: every scenario matches.
  First run the pilot alone on `startup` and `table_roundtrip` and read the JSON with your own eyes; confirm it contains what `tui.py` really shows (the log lines, `KHALI`, `Results -- 3 row(s)`).
- [ ] **Step 3: Mutation check — the comparison can fail.** In `cpp/src/wb_text.cpp` change the NULL cell text to `NULL` (or in `wb_tree.cpp` swap `PK` and `UQ`), rebuild, run `workbench_diff.py`: it must report a
  mismatch in `table_roundtrip` / `tree`. Revert and re-run: green. Record that this was done in the commit message.
- [ ] **Step 4: Leak check.** After a full `ctest` run: no `meradb_test_*` folder left in the temp directory, no `exports/` folder in the build or repo directories, no running `meradb_cli` / `wb_probe` / `python`
  child from the scripts.
- [ ] **Step 5: Commit.**

```
git add cpp/tests/test_wb_e2e.cpp cpp/tests/wb_probe.cpp cpp/tests/workbench_scenarios.json cpp/tests/workbench_pilot.py cpp/tests/workbench_diff.py cpp/tests/CMakeLists.txt
git commit -m "Test the workbench end to end and compare it with the Python workbench through Textual's Pilot"
```

---

### Task 14: Docs, manual terminal checklist, completion checklist

**Files:**
- Modify: `docs/CPP.md`
- Modify: `README.md` (the C++ paragraph around "the workbench is Python-only for now", and any other sentence that says the C++ port has no workbench)
- Modify: `cpp/include/meradb/cli.h` (the header comment: the workbench is no longer "a later phase")

- [ ] **Step 1: `docs/CPP.md` edits**, in this order:
  1. Title: `# MeraDB in C++ (Phases 1-4: engine, server, client, shell, workbench)`.
  2. "What is covered": add a **Phase 4** paragraph (the full-screen workbench: panels, keys, history, CSV, connect dialog, help, syntax highlighting, off-thread statements, behaviour on Ctrl+C / Ctrl+Q);
     replace the line `Not yet (Phases 4-5): the workbench and the polish pass. \`workbench\` prints a note and exits 1.` with `Not yet (Phase 5): the final polish pass.`
  3. Prerequisites: FTXUI is downloaded on the first configure like the other two libraries; `-DMERADB_WORKBENCH=OFF` builds without it (and `workbench` then prints a note and exits 1);
     `-DFETCHCONTENT_SOURCE_DIR_FTXUI=<dir>` for an offline copy of the v5.0.0 tarball.
  4. Command line block: add `meradb_cli workbench [same options as shell]   # also: tui, --tui`, and a sentence that stdin and stdout must be terminals.
  5. New section **### The workbench** after "The shell": layout; a key table (the `tui.py` keys plus the extra keys of R12); how statements run (one worker thread per session, queued keys,
     the busy label); **Ctrl+Q while a statement runs** and **Ctrl+C** (R7, R8) and what happens on a closed window or a kill (crash recovery at the next start); CSV format and location; history is in
     memory only; the connect dialog and Local mode; the help screen is built from `docs/LANGUAGE.md` embedded at build time; colours (truecolor where available, FTXUI down-converts); minimum size 60 x 24;
     exit codes (0 normally, 1 for no terminal / failed open / stub build, 2 usage error).
  6. "Verification against Python": add `python cpp/tests/workbench_diff.py --probe <wb_probe>` (needs Textual; skipped otherwise), `python cpp/tests/gen_highlight_golden.py`, and
     the in-process test names `wbe2e` / `wbui`.
  7. "Layout" table rows for `highlight`, `wb_text`, `wb_editor`, `wb_tree`, `wb_worker`, `wb_session`, `wb_markdown`, `wb_help`, `wb_form`, `wb_panels`, `wb_dialogs`, `wb_ui`, `wb_keys`, `workbench`, `cmake/embed_file.cmake`,
     `sys::TerminalModeGuard`; and the mapping `tui.py` / `highlight.py` -> those files.
  8. "Known divergences": add one bullet per item, worded plainly: **Workbench: editor selection** (R3), **layout and wrapping** (R4: horizontal scroll, character wrap, `▼`/`▶` instead of guide lines),
     **help text embedded at build time** (R9), **colours** (R10, RGB values chosen for Rich colour names), **highlighting** (R11, with the golden lines dropped from the corpus, if any, listed by number),
     **extra keys** (R12), **CSV failure and cell newlines** (R13, R16), **tree cursor kept; local-to-local closes first** (R14), **Ctrl+C never quits; Ctrl+Q waits for a running statement** (R7, R8),
     **needs a terminal; no Python-style dependency message** (R17), **log capped at 20,000 lines** (R16), **minimum size**, and the **mouse** subset (R12/Task 11). Add to "Platform coverage": the workbench has
     been compiled and run on MinGW only; `TerminalModeGuard` on POSIX and FTXUI on MSVC/Linux/macOS are unbuilt.
  9. Manual terminal checklist: add the **Workbench** table below, headed by "Build `cpp/build`, then start the C++ and Python workbenches side by side on separate empty data folders".
  10. "Next phases": mark Phase 4 done with a short paragraph of what Phase 5 should reuse or check (the key probe `wb_keyprobe`, the golden generator, the Pilot comparison; verify FTXUI on Linux/macOS/MSVC;
      the Windows-console findings from the checklist). Remove every sentence saying the workbench prints a note.
- [ ] **Step 2: Manual workbench checklist** (copy into `docs/CPP.md`; running it is for the project owner, results go in the pull request, not the repository):

| # | Terminal | Check | Expect |
|---|---|---|---|
| W1 | Windows Terminal | `meradb_cli workbench --local -D <empty folder>` next to `python -m meradb workbench --local -D <other empty folder>` | Same layout, titles, footer labels and start-up log lines; colours close to the Python screen |
| W2 | Windows Terminal | Resize the window: larger, smaller than 60 x 24, back | Layout follows; the too-small message appears and disappears; no leftover characters |
| W3 | Windows Terminal | Type a two-line statement, `F5`; `Ctrl+R`; select one line with Shift+arrows and `F5`; `Ctrl+A`, `F6` | Results, log and highlighting as in Python; only the selection runs |
| W4 | Windows Terminal, classic console | `Ctrl+Up` / `Ctrl+Down`, `Ctrl+P` / `Ctrl+N`, `F1`, `Esc`, `Tab` / `Shift+Tab`, `PageUp/PageDown`, `Ctrl+S`, `Ctrl+O`, `Ctrl+L` | Every key does what the help screen says; note any key a terminal does not deliver (use `wb_keyprobe`) |
| W5 | Windows Terminal | Type `'é😀नमस्ते日本'` in a string, INSERT, SELECT | Text intact in the editor, log and table; columns stay aligned; Backspace removes a whole emoji |
| W6 | Any | Ctrl+C in the editor; then `SHURU;`, an INSERT, Ctrl+C again | A yellow hint each time; nothing quits; the transaction is still open (header marker) |
| W7 | Any | `SHURU;`, an INSERT, Ctrl+Q; restart the workbench; `DIKHAO * SE t;` | Exit code 0, terminal restored; the row is not there (rolled back); no `RECOVERY` note |
| W8 | Any | A slow statement (cross join of two 3,000-row tables, see the shell checklist): while it runs resize, press `F1`, `Ctrl+L`, queue a second `F5`; then press `Ctrl+Q` | The screen stays responsive, the busy label shows `[chal raha hai +1]`; Ctrl+Q shows `[band ho raha hai ...]`, waits, then exits cleanly; the queued second statement does not run |
| W9 | Windows Terminal | Close the window with the X during a transaction; start again | Same as Python: a `RECOVERY: ...` note on stderr at the next start and the transaction rolled back |
| W10 | Classic console (`conhost`, `WT_SESSION` and `TERM` unset) | Start, use it, quit | Colours and keys work (F-keys, arrows); after exit later commands print normally and the code page / console modes are as before |
| W11 | Linux (xterm / gnome-terminal / tmux) | Start, `Ctrl+S` in the editor, `Ctrl+Q`; afterwards `stty -a` | Ctrl+S does not freeze the terminal; Ctrl+Q quits; `isig` and `ixon` are back on afterwards |
| W12 | macOS Terminal.app and iTerm2 | Same keys; `F5` / `F6` may need "Use function keys" settings; `Ctrl+O` | Note which terminals send what; the aliases (Ctrl+R, Ctrl+P / N) work everywhere |
| W13 | Any | `Ctrl+O` -> Connect to a running C++ server and to a Python server; wrong port; wrong password; `Local mode` | Header shows `host:port`; failures are a red `Connect nahi hua: ...` and the old connection stays; statements work in all combinations |
| W14 | Any | Stop the server (`meradb stop`) while connected, run a statement, then `Ctrl+O` -> Connect again | A red error line and `Ctrl+O se dobara connect karo.`; the UI stays alive; reconnect works |
| W15 | Any | A table with 20,000 rows: `DIKHAO * SE big;`, scroll with arrows / PageDown / End, Right / Left | Smooth; memory reasonable; the title shows `Results -- 20000 row(s)` |
| W16 | Any | Paste 5,000 characters over 100 lines into the editor | Responsive; all lines present; Tab inside is not inserted |
| W17 | Any | Mouse: click each panel, wheel over the log and results | The clicked panel gets the yellow border; the wheel scrolls (optional feature) |
| W18 | Any | `meradb_cli workbench < /dev/null`, `meradb_cli workbench \| cat` | One line on stderr (`Workbench ke liye terminal chahiye ...`), exit code 1, nothing else |
| W19 | Any | `TERM=dumb meradb_cli workbench`, `NO_COLOR=1` | Not supported / ignored: note what happens (FTXUI decides); the program must not corrupt the terminal |

- [ ] **Step 3: `README.md`:** replace "the workbench is Python-only for now" with a sentence that the C++ port includes the full-screen workbench (`meradb_cli workbench`, built with FTXUI; build option
  `MERADB_WORKBENCH`), and keep the pointer to `docs/CPP.md`. Check lines near 83 and 97 (the Python workbench description) stay about Python.
- [ ] **Step 4: `cli.h` comment:** change "The workbench arrives in a later phase; here it says so." to say that `workbench` runs through the hook set by `main()`.
- [ ] **Step 5: Completion checklist below: tick each item with evidence (commands run), then final verification:** full `cmake --build` from scratch in a new directory (zero warnings from our sources),
  full `ctest` (both configurations), `git diff <phase-4-base> -- meradb examples` empty, `git log -p <phase-4-base>..HEAD` shows no attribution trailers or remarks about how the code was produced, and no
  personal details in anything added.
- [ ] **Step 6: Commit.**

```
git add docs/CPP.md README.md cpp/include/meradb/cli.h
git commit -m "Document the C++ workbench, its divergences and the manual terminal checklist"
```

---

## Batches

| Batch | Tasks | What it delivers | Review focus |
|---|---|---|---|
| A | 1-5 | FTXUI behind `MERADB_WORKBENCH`, a library skeleton and key probe; the highlighter checked against `highlight.py`; the pure model pieces (styles, cells, CSV, clipping, history, scroll state, editor buffer, schema tree) | The CMake/offline story; Unicode handling (UTF-8 code points vs terminal cells); golden fidelity; Python's CSV quirks; the tree's expansion rules |
| B | 6-8 | The worker thread and the session controller with every action, tested with a scripted backend; the Markdown subset and the embedded language reference | **Threading** (one thread owns the backend, state changes only through posted closures, quit/shutdown ordering); the order of log lines vs `run_text`; no flaky tests (`--repeat until-fail:20`) |
| C | 9-12 | The FTXUI panels, window, dialogs and mouse; the terminal-mode guard, `runWorkbench` and the CLI hook | GCC argument-order traps in element building; pixel-level colour checks; key routing table; terminal restore on every exit path; the build with the option OFF |
| D | 13-14 | The headless end-to-end test, the Pilot comparison (with its mutation check), docs and the manual checklist | That the comparison can fail; divergence list completeness; honesty about what only a human with a real terminal can verify |

Batches go in order; each ends with a green full suite (`ctest --test-dir cpp/build --output-on-failure`, and a second configure with `-DMERADB_WORKBENCH=OFF` that builds and passes). Within a batch, tasks go in order.

## Phase 4 completion checklist

- [ ] `meradb_cli workbench`, `tui` and `--tui` open the full-screen workbench on a terminal; with stdin or stdout not a terminal they print one note and exit 1; normal exit code 0; argument handling
      identical to `meradb workbench` in Python (same options, same errors, exit 2 for bad options)
- [ ] header (`host:port | db | TRANSACTION`), schema tree (expansion preserved, Enter on db / table / column), results table (zebra, KHALI, green/red booleans, right-aligned numbers, `Results -- N row(s)`, scrolling),
      log (echo, green messages, red errors, `(x.x ms)`, Ctrl+L), editor (highlighting from `highlight`, line numbers, selection-aware F5 / F6), history, CSV with Python semantics, connect dialog, help screen,
      Tab / Shift+Tab, Ctrl+Q — each covered by a `wbui` or `wbe2e` test
- [ ] every database call of a session runs on one worker thread (test: thread ids); the UI never blocks on the database; Ctrl+Q waits for a running statement and rolls an open transaction back; Ctrl+C never
      quits; terminal modes restored on every exit path
- [ ] `workbench_diff.py` matches the Python workbench on all scenarios (or is skipped without Textual), and its mutation check was shown to fail
- [ ] the highlighter equals `highlight.py` on every golden line (with the dropped Unicode-boundary lines, if any, listed in `docs/CPP.md`)
- [ ] `-DMERADB_WORKBENCH=OFF` configures offline, builds, and the whole suite passes there; with it ON the build has zero warnings from our sources
- [ ] FTXUI pinned to v5.0.0 by tarball URL, built with examples / tests / docs / install off, statically linked
- [ ] `docs/CPP.md` updated (workbench section, divergences, verification commands, layout table, Phase 4 hand-off) and the "workbench prints a note" lines removed; README pointer updated; manual checklist W1-W19 written
      (running it is MANUAL)
- [ ] `.hexdump`, the `^` operator and `GINO(ALAG x)` are NOT implemented (project owner's exercises)
- [ ] `git diff <phase-4-base> -- meradb examples` is empty; no attribution trailers, no remarks about how the code was produced and no personal details in any added file or commit message
- [ ] full `ctest` green, no `meradb_test_*` folder, `exports/` folder, process or pid file left behind by any test
