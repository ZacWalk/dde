# dde — Design

How dde is put together and why. [README.md](../README.md) says what it does and how to build it, [AGENTS.md](../AGENTS.md) holds the rules for changing it, and [cpp.md](cpp.md) describes the C/C++ index.

## Goals

1. **Fast and small.** No dependency beyond libdesktop and the standard library. A file loads once into one immutable buffer, and its lines are slices of it until edited.
2. **One obvious mental model.** The left pane lists things to open; the middle pane shows the current document; the Output pane, when it is showing, holds what dd printed last. Everything else is a mode of one of those.
3. **Nothing hidden.** No background writes, no autosave, no telemetry, and nothing runs because a folder was opened. Configuration is written at shutdown.
4. **dd is the build.** dde knows nothing of CMake or the compiler. It runs the project's dd, shows what dd prints, and turns the diagnostics in it into places to go.

## Layers

```
libdesktop (separate repo)      core, platform, render and ui: values, OS services, drawing, controls and views
        ↑
app.cpp / app_state.h           Application: window layout, document index, search, commands, session, dd
        ↑
view_*.h                        Panes: document views, list panels and the Output pane
document.* / document_syntax.*  Text model: a libdesktop text buffer with a path, an encoding and a highlighter
dd_project.* / tool_*.*         dd: what dd.psd1 offers, the runner, diagnostics
cpp_lex.* / cpp_index.*         C/C++ index
json.* / util.* / calc.h        Leaf utilities
```

[libdesktop](https://github.com/ZacWalk/libdesktop) declares four layers in `libdesktop/core.h`, `platform.h`, `render.h` and `ui.h` (`core::`, `platform::`, `render::`, `ui::`); dde links `libdesktop::ui`. `app_state` is both the `ui::app` and the `ui::frame_host` of the one window `ui::create_app_frame` makes, and owns its drawn menu bar, split elements and view controls. The text buffer, the syntax highlighters and the document, Markdown, CSV, hex and list views are libdesktop-ui's ([docs/ui.md](https://github.com/ZacWalk/libdesktop/blob/main/docs/ui.md)): each `view_*.h` names one locally, and dde adds only application policy — the message bar, Escape, zoom and the popup menu — through `ui::view_context` (`app_views.cpp`). Native input and window events arrive through platform callbacks.

### Threading

Documents, the folder index and the views belong to the UI thread. Loading a file, indexing the folder, searching and rebuilding the C/C++ index run on libdesktop's background workers (`platform::run_async`), each on a snapshot taken on the UI thread, and hand their result back with `platform::run_ui`. A worker never dereferences a live `index_item` or `document`, and a generation number discards a result that a newer request has overtaken.

A running dd command adds one reader thread per pipe inside libdesktop's `platform::spawn_child_process`. They own nothing but a byte buffer: each splits its stream into lines and hands every complete line to the UI thread through `platform::run_ui`, behind a shared cancelled flag, so no callback arrives after the process object is gone.

Opening a document is asynchronous, so anything that depends on the loaded text, such as selecting a search match, is passed to `load_doc` as a completion callback. Tests of that ordering run `app_state` on a `deferred_scheduler`, which queues tasks and drains them on `pump()` as the message loop would, rather than completing them inline.

### Invalidation

Views never repaint directly. They set bits in an atomic mask (`invalid::` in `app.h`), and `platform::app::idle()` applies the layout, scrollbar, caret, list population and repaint they ask for once per pump. Search debounce is separate: `on_search` sets a due time and a short `platform::set_frame_delay`, and `frame_tick()` starts the search when the due time arrives.

A document reports three kinds of change to its host, and the difference is what keeps typing cheap:

- `invalidate_lines(start, end)` — **repaint only**, for selection and caret movement. No layout, no highlighting.
- `lines_changed(start, end)` — **the text changed**: those lines are re-wrapped and re-highlighted.
- `line_count_changed(at, delta)` — **lines were inserted or erased**: the view splices its per-line arrays (wrap offsets, row positions, highlight cookies) by `delta` instead of rebuilding them, so pressing Enter costs the same in a 50,000-line file as in a 5-line one.

## Window layout

```
┌───────────────┬─┬───────────────────────────────┬─┬─────────────────┐
│ left pane     │ │ document pane                 │ │ Output pane     │
│ files  OR     │▓│ text / markdown / csv / hex   │▓│ > dd build debug│
│ search        │ │                               │ │ ...             │
│               │ │                               │ │ > exit code 0   │
└───────────────┴─┴───────────────────────────────┴─┴─────────────────┘
```

The Output pane opens when a dd command starts and is toggled with `Ctrl+Shift+U`. Its splitter divides only what is left of the document pane, so the two can never cross; with no room left it collapses against the right edge rather than leaving the window.

`view_mode` is the cross product of `view_content` (`edit_text`, `markdown`, `csv`, `hex`) and search panel on or off. `app_state::set_mode` swaps the document pane's control for the matching view and points the left pane at the file list or the search panel.

## Documents

A `document` is libdesktop-ui's `text_buffer` plus what makes it a file: a path, an encoding, line endings, loading and saving (`document.h`).

- **Loading** reads at most 2 MB. A larger file keeps its first 2 MB, cut at a character boundary, and opens read-only; a truncated document refuses to save, so the rest of the file cannot be lost. A file with a binary extension or binary content opens in the hex view. UTF-16 is decoded to UTF-8 once, on load.
- **Lines** are UTF-8 and slices of the loaded bytes until edited, when they own their text. All coordinates (`text_location.x`) are **UTF-8 byte offsets**, not columns or codepoints; display columns come from a per-line cache that expands tabs.
- **Undo** is a linear list of grouped insert and erase steps with a redo cursor. `undo_group` is an RAII scope that commits on destruction, and undoing back to the last save marks the document clean.
- **Saving** writes a temporary file and replaces the original with it. The encoding found on load goes back out: UTF-16 stays UTF-16 in its byte order, a byte-order mark is kept or left out as it was, and line endings are preserved. Before overwriting, a save checks the file's timestamp and asks if it changed on disk.
- **Residency.** Documents live in the folder index. Switching files never prompts; unsaved documents stay in memory with their undo history and show red in the folder browser. dde asks to save only when the root folder changes, on exit and before a dd command. Unmodified documents past the 24 most recently opened are dropped and reloaded on demand; modified, read-only and generated documents never are.

Highlighters are libdesktop-ui's (`ui/syntax.h`): per line, carrying a 32-bit cookie from the line before, so an edit re-highlights from where it happened rather than from the top. `document_syntax.cpp` is dde's choice of highlighter: by extension for text — C++, Rust, Python and PowerShell — Markdown's for Markdown, and the plain one for CSV and hex, which draw their own text.

## Views

```
view_base                       scroll offset and content extent
├── text_view                   fonts, message bar, clipboard, zoom, Escape
│   └── doc_view                document, caret, selection, word wrap, hit-testing, painting
│       ├── edit_doc_view       writable: the document pane
│       └── read_only_doc_view  no caret, no horizontal scroll, word wrap fixed, keys scroll
│           ├── markdown_doc_view
│           ├── csv_doc_view
│           ├── hex_doc_view
│           └── output_view     the dd Output pane
└── list_view                   rows, selection, hover, keyboard navigation
    ├── file_list_view          folder tree, inline rename, drag and drop
    └── search_list_view        search box, grouped results
```

Everything but `output_view` and the two list panels is libdesktop-ui's, named by a one-line header. A list row is a `ui::list_item`; the panels hang their own object off its `data` — an `index_item` in the folder browser, a search hit in the search panel — and the shared list never looks inside it.

The split between `edit_doc_view` and `read_only_doc_view` keeps the read-only panes predictable: no caret, no horizontal scrolling, no word-wrap toggle, and arrow keys that scroll rather than move an invisible cursor.

## Panes

| Pane | Purpose | Opens with | Leaves with |
|---|---|---|---|
| **Folder browser** | Navigate the root folder; create, rename and delete files | default | — |
| **Search** | Live text search across the root folder | `Ctrl+Shift+F` | `Escape` |
| **Text editor** | The only place text can be changed | default | — |
| **Markdown preview** | Read rendered Markdown | automatic for `.md` and `.markdown`; `Ctrl+M` on any text | `Escape` |
| **CSV table** | Read `.csv` as an aligned table | automatic for `.csv` | `Escape` |
| **Hex** | Read binary files | automatic for binary content | — |
| **Output** | What the last dd command printed, live | a dd command, `Ctrl+Shift+U` | `Escape` returns the keyboard to the editor |

Each document remembers its own content view, so switching away and back restores what you were looking at.

### Folder browser

Click or arrow-key to preview a file, with focus staying in the list; `Enter` opens it and moves focus to the editor. Clicking a folder expands or collapses it. Right-click offers New File, New Folder, Copy Path, Rename (`F2`) and Delete (to the Recycle Bin, after asking). A right-click inside a Shift-selected run keeps the run, so Copy Path takes every row in it; Rename and Delete act on one file, so a run offers neither. New items are named `new-file.md` and `new-folder`, suffixed `-2`, `-3`, … until unique. Names are refused when they hold a path separator or another character Windows forbids, are `.` or `..`, end in a dot or a space, or name a reserved device. Files dropped on the panel are copied in.

### Folder index

Indexing is recursive and runs off the UI thread. `.git` is always skipped, and every `.gitignore` met on the way down applies to the subtree below it, so build output stays out of the browser, out of search and out of memory. The matcher (`gitignore.h`) covers the subset `.gitignore` files use; it does not match character classes, so a pattern with one hides nothing rather than hiding the wrong files.

### Search

Typing runs a live search 150 ms after the last keystroke, and a newer query cancels the one in flight. A longer query can only match where a shorter one did, so a query that extends the previous one rescans only the files that matched it; any edit or reindex disables that narrowing until the next full scan.

Results are grouped under a collapsible header per file, with the relative path and match count. Selecting a match opens the file and selects the match once the file has loaded; focus stays in the panel until `Enter`. Search stops at **5,000 results** (the count then reads "limit reached"), skips files over **10 MB** and binary files, and keeps at most **400 bytes** of context around each match, so a minified file with megabyte-long lines costs neither memory nor paint time.

### Text editor

Unlimited per-document undo. Word wrap (`Alt+Z`) is application state, applied to every editable view and remembered. `Tab` and `Shift+Tab` indent and unindent; `Ctrl+R` reformats JSON; Edit ▸ Sort & Remove Duplicates sorts the lines byte-wise and drops repeats; `Ctrl+E` replaces a selected arithmetic expression with its value. Double-click or `Ctrl+click` selects a word, the left margin selects lines, and `Ctrl+click` in the margin selects everything. Dragging a selection moves it, or copies it with `Ctrl` held; `Escape` cancels the drag. The caret's row carries a highlight band while the editor has focus.

### Markdown, CSV and hex

The **Markdown** preview is `ui::markdown_view`. It draws the source, markers and all, laid out from `ui::md`'s parse — headings, emphasis, code, links, lists and tables — so selection works as in the editor and what is selected is what is in the file. dde does not follow links.

The **CSV** table is `ui::csv_view`: RFC 4180 quoting, aligned columns under a header row, width-capped columns with wrapped cells and numbers right-aligned. Copying gives the records as they are in the file.

The **hex** view shows an 8-digit offset, 16 bytes and their ASCII on each row.

## dd integration

dde builds nothing itself. It runs the project's own dd — the vendored `dd.ps1` in the root folder — and shows what dd prints. Nothing here is a linked dependency: `pwsh` and the toolchain dd drives are the user's own. dd's commands and exit codes are documented in [its CLI reference](https://github.com/ZacWalk/dd/blob/main/docs/cli.md#commands).

### Discovery is data only

When the root folder holds `dd.ps1`, the dd menu comes alive. When it also holds `dd.psd1`, `dd::manifest_probe` reads it — as data — with `pwsh -NoProfile -NonInteractive` running `Import-PowerShellDataFile -LiteralPath ./dd.psd1 | ConvertTo-Json` from the root. No path is spliced into PowerShell source, and `Import-PowerShellDataFile` refuses anything that is not a literal, so a manifest with code in it is reported as unreadable, not run. The project's `dd.ps1` and `.dd` scripts never run because a folder was opened.

`dd::parse_manifest` is a pure function from that JSON to a `dd::project`: the project's name and default target; each target's id, kind, test label and whether it runs on this host; and each command's name, description, effects, dry-run support and parameters. `ConvertTo-Json` writes a one-element list as the element itself, so every list also accepts a single value. Names are held to dd's own rules, which also means a name can never be mistaken for an option. A manifest that cannot be read leaves dd's own verbs, not an error.

The probe has a runner of its own, so reading the manifest never waits behind a build. It runs when the folder is indexed, again when `dd.psd1`'s timestamp has moved — after a refresh, or after saving it in dde — and on dd ▸ Refresh dd. A reply about a folder that is no longer open is dropped.

### One execution path

Every dd command is

```text
pwsh -NoProfile -NonInteractive -File <root>\dd.ps1 <verb> <args...> --non-interactive
```

run as an argument array with the root as its working directory — never an assembled shell string, never `-ExecutionPolicy`. `dd::command_line` is the only place that builds one. `tools::runner` runs one at a time; another request waits its turn in a bounded queue, and a full queue is reported, not dropped silently. A modified document prompts save, discard or cancel before anything runs, so a build never compiles stale text, and Clean and project commands that write ask every time. Stop ends `pwsh` and every process it started — libdesktop runs it in a Windows job object — and drops anything waiting.

### The menu and its keys

| dd menu | Runs | Key |
|---|---|---|
| Build | `build <configuration>` | `F7`, `Ctrl+Shift+B` |
| Test | `test`, with `--label <label>` when one is chosen | `Ctrl+Shift+T` |
| Run | `run <target>`: the default target, else the only runnable one | `Ctrl+F5` |
| Launch | `launch <target>` | `Ctrl+Shift+F5` |
| Run Target ▸, Launch Target ▸ | the same for each runnable target, when there is more than one | |
| Configuration ▸ | Debug, Release or Both, for Build and Clean; remembered, Debug by default | |
| Test Label ▸ | All, or one of the targets' test labels | |
| Project Commands ▸ | each command `dd.psd1` declares | |
| Format | `fmt` | |
| Clean... | `clean <configuration> --yes`, after asking | |
| Doctor, Targets, Dependencies | `doctor`, `targets`, `dep list` | |
| Next / Previous Diagnostic | | `F8` / `Shift+F8` |
| Stop | the running command, and any waiting behind it | `Shift+F5` |
| Refresh dd | reads `dd.psd1` again | |

A target is runnable when it is not a library and its platforms, if it lists any, include `x64-windows`; Run and Launch are disabled for a project with nothing runnable. A project command declared `effects = 'read'` runs as it is. One that writes asks first and passes `--yes`, and when it supports `--dry-run`, No previews it instead. dde never asks for a parameter's value, so a command with a parameter it cannot fill — no default, and either required or more than a flag that can stay off — is shown disabled with what it needs, `layout (needs --width, --dump, --file)`, to be run from a terminal. Generated items take no key, so no manifest can add a conflict.

Without `dd.ps1` every item is disabled and the menu's first line says why, as it does when `pwsh` is not on `PATH`, while `dd.psd1` is being read, and when it could not be.

### The Output pane

The right-hand pane is a read-only `output_view` over a document of its own. Starting a command clears it and opens the pane if it is hidden, without taking the keyboard. The first line is the command, `> dd build debug`; each line follows as it arrives; the last is a summary — exit code, elapsed time, errors and warnings, and any failed Ninja targets. The pane follows the newest line only while its reader is at the bottom, so scrolling up to read an error is not undone by the next line. Each line is coloured by what it is: the command and its summary, dd's own `dd: ` progress, an error or a warning.

The document and its view report to a sink of their own, so output can neither touch the editor's wrap cache nor repaint it. A line costs an append and a splice of the view's per-line arrays, and no layout at all while the pane is hidden. The pane keeps what the runner keeps — the first 2,000 lines, a `... N lines omitted ...` marker and the newest 2,000 — dropping from the middle 500 at a time so that a long build is not a memmove per line. The message bar shows the newest line, behind Ninja's `12/345` progress.

Double-click or `Enter` on a line that names a file opens it at that line and column, with the keyboard in the editor. A file the folder index leaves out — a fetched dependency, a sibling checkout — joins the folder browser rather than moving the root away from the project.

The pane belongs to the open project: opening another folder empties it and forgets its diagnostics, so `F8` cannot lead back into the project it replaced. A command still running when the folder changes keeps its output, and its diagnostics stay rooted where it ran.

### Diagnostics

`tool_output.cpp` turns the merged stream into diagnostics with hand-written matchers over `string_view`, no `std::regex`: MSVC, clang and gcc, the linker, CMake's indented error blocks, Ninja progress and `FAILED:` targets, CTest failures and PowerShell error records. Escape sequences are stripped first, since tools colour their output even when it is redirected. A `note:` attaches to the diagnostic above it, as does the indented block under it — MSVC's `with [ _Ty=int ]` template context belongs to the note it follows. A line that matches nothing is kept verbatim; nothing is ever dropped.

Diagnostics are known as they arrive, so `F8` and `Shift+F8` work while the build is still running. They step through them in both directions, wrapping, select the Output line each came from, and open the file with the caret on the reported line and column. A diagnostic that names no line — a linker error naming an object file — reports in the message bar and moves nothing. When a command finishes, the document on screen is reloaded if the command changed it on disk and it has no unsaved changes.

## C/C++ navigation

Go to Definition (`F12`), Switch Header/Source (`Ctrl+F12`) and Back/Forward (`Alt+Left`, `Alt+Right`) are described with the index behind them in [cpp.md](cpp.md).

## Commands and keyboard

One `std::vector<command_def>` (`app_state::make_commands`) is the single source of truth for the menu bar, the enable and check state, the accelerators and the generated About document. Each entry owns its own lambda; adding a command is one table entry ([AGENTS.md](../AGENTS.md#adding-a-command)). Commands report through the message bar at the top of the document pane.

There is exactly one accelerator dispatcher: `ui::dispatch_accelerator` walks the drawn menu and calls the command. Every binding lives in the table, secondary ones included as a command's `accel_alt`, and a test holds every binding unique. A view handles only keys the table does not claim — caret movement, `Backspace`, `Tab`. Commands are focus-aware rather than focus-routed: each asks what has focus and acts, instead of re-dispatching a keystroke to another control.

- **Clipboard and delete** (`Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `Delete`, `Ctrl+A`) act on whatever has focus — the editor, the Output pane, the file list, or the search or rename box. An inline edit box wins for copy and cut only when it has a selection, so `Ctrl+C` in the search panel still copies the selected result's path. Switching to another application and back returns the keyboard to the pane that had it.
- **Text-editing commands** (undo, redo, reformat, sort, calculate, spell check) need the text editor: they are disabled while an inline edit box has focus, while a preview is showing and on a read-only document. `document::replace_text` also refuses a read-only document, so no path can bypass the check.
- **Save and Save As** are disabled on a read-only document, and `document::save_to_file` refuses one outright, so a truncated file can never be written back over the original.

`F5` refreshes the focused panel: the search when the search panel has focus, the folder index otherwise. `F3` and `Shift+F3` step through search results and are disabled outside the search panel; `F8` and `Shift+F8` step through dd's diagnostics, or through the search results while the search panel is active, and neither opens the search panel. `Alt+Z` is disabled outside the editor.

Help ▸ About (`F1`) generates the authoritative list of commands and their keys from the command table, and Help ▸ Run Tests (`Ctrl+T`) runs the unit tests into a document; both documents are read-only and never dirty, and appear in the folder browser so you can return to them. The keys the table does not hold:

| | |
|---|---|
| **Caret** | arrows · `Ctrl+←/→` word · `Home`/`End` · `Ctrl+Home`/`Ctrl+End` · `PageUp`/`PageDown` · `Ctrl+↑/↓` scroll only · add `Shift` to extend the selection |
| **Editing** | `Tab` / `Shift+Tab` indent · `Backspace` · `Ctrl+Backspace` delete word left |
| **Zoom** | `Ctrl++` / `Ctrl+-` / `Ctrl+Wheel` zoom the editor, the panels and the Output pane separately |
| **Read-only views** | arrows, `Home`/`End`, `PageUp`/`PageDown` scroll · `Escape` returns Markdown and CSV to the editor |
| **Output** | `Enter` or double-click opens the file a line names · `Escape` returns the keyboard to the editor |
| **Panels** | `↑`/`↓` navigate and preview · `Enter` open, or expand or collapse a folder or search group · `F2` rename · `Delete` delete file · `Escape` close search |

## Spell check

Three modes — `auto_detect` (the default, active for `.md` and `.txt`), `enabled` and `disabled` — remembered in configuration and toggled with `Ctrl+Shift+P`. Misspellings are underlined in the editor; right-click offers suggestions and *Add to Dictionary*. The platform checker is created on first use, and spell check is silently inactive when none is available.

## Configuration

`dde.ini` lives beside the executable when that folder is writable, otherwise in the user's temp folder. It is written at shutdown, so no command-line mode ever writes it.

| Section | Keys |
|---|---|
| `[]` | `window_rect`, the window placement libdesktop saves |
| `Font` | `TextSize`, `ListSize` (8–72) |
| `Splitter` | `PanelRatio` (0.05–0.95, default 0.2) |
| `Output` | `Visible`, `SplitterRatio` (0.05–0.95), `FontSize` (8–72) |
| `dd` | `Configuration` (`debug`, `release` or `both`) |
| `View` | `WordWrap`, `SpellCheck` |
| `Recent` | `Folder`, `Document` |
| `RecentFolders` | `Folder1`–`Folder8`, `Document1`–`Document8` |

A restored root folder that no longer exists is skipped, and the current directory is used instead. A restored document reopens even when the folder index leaves it out. A file named on the command line opens instead of the restored document.

## Command line

| | |
|---|---|
| `dde-64.exe /test` | Run the unit tests to stdout; exit 0 on success, 1 on any failure. |
| `dde-64.exe /spell:<word>` | Print spell-checker diagnostics and suggestions. |
| `dde-64.exe /dd[:<folder>]` | Read the folder's `dd.psd1` — the current directory's by default — through a real `pwsh`, as the dd menu does, and print the project, targets, test labels and commands dde found. Exit 1 when there was nothing to read. Runs no project script. |
| `dde-64.exe <path>` | Open a file. Arguments starting with `/` or `-` are ignored; the last plain argument wins. |

The first three open no window. Both `/x` and `--x` forms are accepted.

## Known limitations

- UTF-32 files are not decoded.
- A cell is drawn padded into its column, so dragging selects whole records in a CSV table and whole source lines in a Markdown table. The hex view does not drag-select, though `Ctrl+A` copies it.
- A CSV record is one line: a quoted field with a line break in it draws its second half as a new record.
- Search folds case one codepoint at a time, so `ß` does not match `SS`, and characters beyond U+FFFF compare case-sensitively.
- dd writes its output in `pwsh`'s console code page, which dde reads as UTF-8, so a non-ASCII character in a build line can arrive garbled.
