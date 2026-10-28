# MeraDB Desktop: a native Hinglish workbench window

A native desktop application, `meradb_gui`, that is the graphical sibling of the terminal workbench. It is
**our own Hinglish workbench**: every label, button, tooltip, empty state, dialog and message is in Hinglish,
using the vocabulary the terminal workbench (`meradb/tui.py`, `cpp/src/wb_*`) already uses. The look is
minimal and monochrome, in the style of Vercel's Geist design system. It is a C++-only extra (the Python program
has no desktop app).

Stack: **Dear ImGui** (immediate-mode UI) on **GLFW** + **OpenGL 3**, fonts **Geist** and **Geist Mono** (SIL Open
Font License) embedded in the binary. All fetched by CMake `FetchContent` from pinned release tarballs, like
nlohmann/json, Catch2 and FTXUI. No browser, no web view.

## Goals and non-goals

- Feature parity with the terminal workbench: schema tree, query editor with MeraDB syntax highlighting, results
  grid, log, history, SAMJHAO (explain), CSV export, connect dialog (server or local mode), language reference,
  transaction indicator, safe quit (running statement finishes, open transaction rolls back).
- Reuse, do not copy: `wb::Session` (wb_session.h) is the controller and model. The GUI is a new view over it,
  exactly as the FTXUI workbench is. `TextBuffer`, `TreeModel`, `LogBuffer`, `History`, `makeCell`, `csvDocument`,
  `highlight::spans`, `helpMarkdown()` are all reused.
- Non-goals: query tabs, multiple connections at once, editing cells in the grid, charts.

## Libraries and build

- `MERADB_GUI` CMake option, default ON on Windows and macOS. On Linux default ON, but if the X11/OpenGL
  development files GLFW needs are missing, configure prints one clear message and skips the GUI (the rest builds).
- **Split the FTXUI-free workbench model into its own static library** `meradb_wbmodel` (wb_text, wb_editor,
  wb_tree, wb_worker, wb_session, wb_markdown, wb_help and the embedded LANGUAGE.md). `meradb_workbench` (FTXUI) and
  `meradb_gui` both link it. `MERADB_GUI=ON` with `MERADB_WORKBENCH=OFF` must work. No behaviour change for the
  terminal workbench; its tests keep passing unchanged.
- Pinned dependencies (verify each URL and tag exists before using it; record the exact versions in
  `THIRD_PARTY_NOTICES.md` with licence and upstream URL):
  - GLFW 3.4 (zlib licence): build with docs, tests and examples off.
  - Dear ImGui, a recent tagged release (MIT): compile `imgui*.cpp` plus `backends/imgui_impl_glfw.cpp` and
    `backends/imgui_impl_opengl3.cpp` into a static library; mark its headers SYSTEM.
  - Geist and Geist Mono static TTFs (OFL 1.1) from the official vercel/geist-font release: Regular and Medium of
    Geist, Regular of Geist Mono. Embed them as byte arrays at build time (extend `cmake/embed_file.cmake`
    with a variable name parameter; it must handle binary files).
  - stb_image_write (public domain / MIT) for the screenshot option below, from a pinned nothings/stb commit.
- Windows: `meradb_gui` is a WIN32-subsystem executable (no console window when double-clicked), statically linked
  like `meradb_cli`, with an `.ico` application icon and a version resource. Console output (for `--help` and
  errors) uses `AttachConsole(ATTACH_PARENT_PROCESS)` when started from a terminal; otherwise errors go to a message
  box.
- OpenGL: GL 3.0 + GLSL "#version 130" on Windows and Linux, GL 3.2 core + "#version 150" + forward-compatible on
  macOS (the standard ImGui example setup).
- `-Wall -Wextra` clean on our own code; third-party code compiled with warnings off and included as SYSTEM.

## Command line

```
meradb_gui [-h] [-D DATA] [-H HOST] [-p PORT] [-d DATABASE] [-W] [-U USER] [--local] [--version]
```
Same meaning as `meradb workbench` (reuse `parseCliArgs` / `openBackend`; usage and help text name the program
`meradb_gui`). Started with no options (double-click) it behaves like `meradb workbench`: connect to a running
server, else local mode. `meradb_cli gui` starts `meradb_gui` from the same folder with the same options (and says
clearly if it is not there).

Hidden test options (documented in docs/CPP.md, not in `--help`):
- `--screenshot FILE.png` renders with a hidden window: runs `--script TEXT` (if given) through the session, waits
  until the worker is idle, draws a few frames, writes the window contents to the PNG, exits 0.
- `--theme light|dark` and `--size WxH` (default 1280x800) for screenshots.

## Design

Monochrome Geist look. Dark is the default; light is a full equal theme. The theme follows the system setting
(Windows: `AppsUseLightTheme`; macOS: `AppleInterfaceStyle`; else dark), toggled from the top bar.

| Token | Dark | Light |
|---|---|---|
| background | `#000000` | `#FFFFFF` |
| surface (panels, inputs) | `#0A0A0A` | `#FAFAFA` |
| border (1px hairlines) | `#262626` | `#EAEAEA` |
| hover / selected row | `#1A1A1A` | `#F2F2F2` |
| text | `#EDEDED` | `#171717` |
| muted text | `#A1A1A1` | `#666666` |
| faint (line numbers, KHALI) | `#5E5E5E` | `#A8A8A8` |
| primary button | bg `#EDEDED`, text `#0A0A0A` | bg `#171717`, text `#FFFFFF` |
| focus ring / selection | `#0070F3` (selection at 35% alpha) | same |
| error | `#FF6166` | `#E5484D` |
| success | `#62C073` | `#297A3A` |
| warning (transaction badge) | `#F2A65A` | `#AD5700` |

Syntax colours (Geist code style), dark / light: keyword `#FF4D8D` / `#C41562`, string `#62C073` / `#297A3A`,
number and boolean and KHALI-constant `#52A8FF` / `#0068D6`, comment `#A1A1A1` / `#666666`, function
`#BF7AF0` / `#7820BC`, type `#F2A65A` / `#AD5700`, operators and brackets = text colour.

Type: Geist 14px for the interface (Medium for headings and buttons), Geist Mono 13px for the editor, the results
grid and the log. Scale every size by the monitor content scale (`glfwGetWindowContentScale`) and rebuild fonts when
the window moves to a monitor with a different scale. Radius 6px on buttons and inputs, 8px on dialogs; no shadows,
no gradients. Spacing on a 4px grid.

Layout (all splitters draggable; sizes kept for the session):
```
+---------------------------------------------------------------------------------------+
| (mark) MeraDB  /  local (C:\...\data)  /  college   [TRANSACTION]   Connect  Madad  (o)| 48px top bar
+--------------------+------------------------------------------------------------------+
| Schema             |  1  DIKHAO naam, umar                                           |
| [ dhoondo...    ]  |  2  SE students                                                 | editor
|  v college         |  3  JAHAN umar > 18;                                            |
|    v students      |                                                                  |
|        id    INT PK|  [Chalao  Ctrl+Enter]  [Samjhao]          Pichli  Agli   chal raha hai...|
|        naam  TEXT  +------------------------------------------------------------------+
|    > courses       |  Results   Log                         12 rows, 3.1 ms   [CSV]  | tabs
|  > main            |   #   id   naam      umar                                        |
|                    |   1    1   Asha        19                                        | grid
+--------------------+------------------------------------------------------------------+
| local mode  -  db: college  -  meraDB 1.0.0                                            | 24px status bar
+---------------------------------------------------------------------------------------+
```
The memorable element is the top bar: a breadcrumb of connection / database in Geist, with the transaction badge
appearing there when SHURU opens one. Everything else stays quiet.

## Hinglish copy (use these words; reuse existing workbench strings where they exist)

- Window title: `MeraDB Workbench`. Panels: `Schema`, `Query`, `Results`, `Log`.
- Buttons: `Chalao` (run), `Samjhao` (explain), `Pichli` / `Agli` (history), `CSV` (export), `Connect`,
  `Madad` (reference), `Log saaf karo`, `Bahar` (quit, in the window menu only), dialog: `Connect`, `Local mode`,
  `Cancel` (as in tui.py's ConnectScreen).
- Placeholder in an empty editor: `Yahan query likho, jaise:  DIKHAO * SE students;   (Ctrl+Enter se chalao)`.
- Empty results: `Abhi koi result nahi. Query likho aur Chalao dabao.` Empty log: `Log khaali hai.`
- Schema filter placeholder: `dhoondo...`. No databases: `Koi database nahi.`
- Busy: `chal raha hai...`. Quitting: `band ho raha hai...` (waits for the running statement).
- Transaction badge: `TRANSACTION`; tooltip: `PAKKA se save karo, WAPAS se cancel.`
- CSV saved toast: `N row(s) CSV mein save: <path>` (the existing log text) with a `Folder kholo` button.
- Theme toggle tooltip: `Light / dark`. Results header: `N row(s), X ms`.
- Engine messages, errors and the log keep their existing Hinglish text from the Session (do not translate).

## Behaviour

- Keys (also shown in the Madad window): Ctrl+Enter or F5 = Chalao (the selection, else all text); Ctrl+Shift+Enter
  or F6 = Samjhao; Alt+Up / Alt+Down and Ctrl+Up / Ctrl+Down = Pichli / Agli; Ctrl+S = CSV; Ctrl+O = Connect;
  F1 = Madad; Ctrl+L = Log saaf karo; Ctrl+Q or closing the window = Bahar; Esc closes a dialog; Ctrl+K opens the
  command menu (below). macOS uses Cmd for Ctrl.
- Command menu (Ctrl+K): a small centred palette with a filter box listing actions (Chalao, Samjhao, CSV, Connect,
  Madad, Log saaf karo, theme), every table (`DIKHAO * SE t SIRF 100`) and the last 20 history entries; Enter runs
  the highlighted item.
- Schema tree: click selects; double-click or Enter activates exactly like `TreeModel::activate`
  (database: `ISTEMAL`; table: `DIKHAO * SE t SIRF 100`; column: insert its name into the editor). Right-click
  menu: `Top 100 rows dikhao`, `Naam editor mein daalo`, `Naam copy karo`. The filter box hides non-matching tables
  and columns (case-insensitive).
- Editor: a custom widget drawn with ImDrawList over `wb::TextBuffer` (no third-party editor). Line numbers,
  current-line tint, selection, blinking caret, highlighting from `highlight::spans` per visible line only (long
  lines stay cheap: only visible columns are drawn), mouse click / drag / double-click word / triple-click line /
  shift-click, keyboard movement (arrows, Home/End, Ctrl+arrows by word, PageUp/Down, Ctrl+Home/End), Ctrl+A,
  Ctrl+C/X/V through ImGui's clipboard, Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) undo and redo (an undo stack of text +
  cursor snapshots in the GUI layer, typing merged into one step until a pause or a non-typing key), Tab inserts
  4 spaces (TextBuffer rule), both scroll bars, text input from `io.InputQueueCharacters` (UTF-8, including
  characters outside the BMP), IME position set to the caret.
- Results grid: `ImGuiTable` with `ImGuiListClipper` (100,000 rows stay smooth), frozen header row and row-number
  column, resizable columns, a horizontal scroll bar, Geist Mono cells; `KHALI` in faint, `SACH` / `JHOOTH` in
  success / error, numbers right-aligned in the number colour; long cells clipped with the full text in a tooltip;
  click selects a cell, Ctrl+C copies it, Ctrl+Shift+C copies the row tab-separated.
- Log: the `LogBuffer` entries in Geist Mono with their kinds mapped to the tokens (error, message/connected =
  success, echo = highlighted query, dim = muted); follows the bottom unless the user scrolled up.
- Busy: while the worker runs, the status shows `chal raha hai...`; the UI never blocks; keys queue as the Session
  allows. The app redraws only on events plus a slow tick while busy (glfwWaitEventsTimeout), so it idles at ~0% CPU.
- Connect dialog: modal, fields `Host`, `Port`, `Password (agar server par hai)` (masked), `Database (optional)`,
  defaults from `Session::connectDefaults()`; buttons `Connect`, `Local mode`, `Cancel`; Enter = Connect, Esc =
  Cancel; outcome goes through `Session::connect` (its log message tells success or failure).
- Madad window: the keys table plus `docs/LANGUAGE.md` from `wb::helpMarkdown()`, drawn by a small ImGui
  Markdown renderer (headings in Geist Medium, paragraphs wrapped, `code` and fenced blocks in Geist Mono on the
  surface colour, tables as ImGui tables, bullet lists, bold). Must not hang on any input (reuse the lessons of
  wb_markdown's tests).
- Quit: Ctrl+Q / window close calls `Session::requestQuit()`, shows `band ho raha hai...`, and leaves the loop from
  `onExit` (glfwSetWindowShouldClose is overridden until then). Exit code 0.
- Threading: the Session's `UiPoster` pushes closures into a mutex-protected queue and calls
  `glfwPostEmptyEvent()`; the main loop drains the queue each frame. The Session alive token makes late closures
  safe.

## Tests

- Unit (Catch2, no window): theme tokens to ImGui colours, key mapping, undo stack, the editor's position <->
  pixel mapping and hit testing (with a fixed fake glyph width), the tree filter, the command menu filter, the
  markdown renderer's block parser (and that it terminates on hostile input), the poster queue.
- Headless UI tests: create an ImGui context without a window or renderer (display size set by hand, font atlas
  built from the embedded fonts), run real frames of the whole UI against a `LocalBackend` in a temp
  `MERADB_DATA`, feed keys and mouse through `io.AddKeyEvent` / `AddMousePosEvent` / `AddInputCharacter`, and
  check the Session state: typing + Ctrl+Enter runs and shows rows, Samjhao, history, CSV file written to a temp
  folder, connect dialog local mode, tree activation, Ctrl+Q path closes the backend and rolls back an open
  transaction. No ImGui ID conflicts or asserts (enable `IM_ASSERT` to throw in tests).
- Screenshot test (ctest, skipped with exit code 77 when no display/OpenGL is available): run
  `meradb_gui --local --screenshot out.png --script "..."` in a temp data dir for both themes; check the PNG
  exists with the requested size and is not a single colour.
- Everything with temp `MERADB_DATA` folders and high ports only; never the real per-user data folder.

## Batches

- **A Foundation**: dependencies and CMake (wbmodel split, GLFW, ImGui, fonts, stb), window bootstrap, DPI fonts,
  theme tokens and system theme detection, poster queue, main loop with idle waiting, `--screenshot`, CLI options,
  `meradb_cli gui`, empty layout with top bar and status bar, headless test harness. Exit: a screenshot of the empty
  layout in both themes.
- **B Panels**: editor widget (+ undo), schema tree with filter and context menu, results grid, log, tabs,
  splitters, toolbar, keys. Exit: headless tests of the full query flow; screenshots with data.
- **C Dialogs and polish**: connect dialog, Madad window with markdown renderer, command menu, CSV toast, busy and
  quitting states, Windows icon and version resource, WIN32 subsystem console handling.
- **D Verify and document**: Linux build (WSL, Xvfb or WSLg for the screenshot test), docs/CPP.md section, README,
  manual checklist (high-DPI monitor, light/dark switch, IME, clipboard, very long lines, 100k rows, server
  dropped mid-query, quit with open transaction).
