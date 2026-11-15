# AGENTS.md

dde is the editor for [dd](https://github.com/ZacWalk/dd) projects. [README.md](README.md) says what it does and how to build it; [docs/design.md](docs/design.md) says how it works — read it before a structural change and update it when you change high-level behaviour; [docs/cpp.md](docs/cpp.md) covers the C/C++ index.

## Non-negotiables

1. **Windows-only code lives in libdesktop.** This repository talks only to the `core::`, `platform::`, `render::` and `ui::` abstractions in `libdesktop/core.h`, `libdesktop/platform.h`, `libdesktop/render.h` and `libdesktop/ui.h`, from the shared [libdesktop](https://github.com/ZacWalk/libdesktop) repository. No `windows.h`, Win32 calls or types, `HWND`/`HDC` or Win32 constants here. A new OS capability goes into libdesktop first, under its own [AGENTS.md](https://github.com/ZacWalk/libdesktop/blob/main/AGENTS.md); then push it and bump `GIT_TAG` in `CMakeLists.txt`, or CI and a fresh clone build against the old revision. The other apps build against the same headers, so a signature change breaks them until they bump their own pins.
2. **Build with dd.** In PowerShell 7.4+, `.\dd.ps1 build debug`; `build` alone builds both configurations. Change libdesktop in a sibling `..\libdesktop` checkout, which the build picks up ([README.md](README.md#building)). Add no other dependency.
3. **Run the gate.** `.\dd.ps1 test --label unit` builds both configurations and runs the `/test` suite through CTest without a window; CI runs the same. Unfiltered `test` also starts the app for dd's desktop smoke check. Add a test for every behaviour you fix — `tests.cpp`, or `tests_cpp.cpp` for the index, the tool runner and the output parsers — and run libdesktop's suite after touching it.
4. **Optimise for small and fast.** This is the point of the project. No per-keystroke or per-paint allocation, no O(document) work for a local edit; prefer `string_view` and reusable buffers. Delete dead code rather than leaving it.
5. **Temporary files go in `tmp/`.**

## Conventions

- Modern C++ in `snake_case`. Convert Win32 Hungarian names (`nCrlfStyle`, `bClearModifiedFlag`) when you touch them; add no more.
- `.clang-format` is the layout: tabs, Allman braces, 120 columns. `.\dd.ps1 fmt` applies it to `src` (dd formats only `include`, `src` and `tests`), given clang-format on `PATH`, the only place dd looks; Visual Studio installs one in `VC\Tools\Llvm\x64\bin`. A hand-laid table — the command table, a run of one-line key dispatches, test data — is fenced with `// clang-format off` and `// clang-format on`.
- dde builds at `/W4` with no warnings in Debug and Release, and `.clang-tidy` names the bug-finding and performance checks the code is kept clean against; a deliberate exception is a `// NOLINTNEXTLINE(check): reason` above its line.
- Text coordinates are **UTF‑8 byte offsets**, never codepoints or columns. Step with `core::utf8_prev` / `core::utf8_next`; never `++`/`--` a byte index over text that may be non-ASCII.
- Views never repaint directly. Raise an `invalid::*` bit and let `platform::app::idle` coalesce the work. Live search is debounced by `app_state::frame_tick`, through `platform::set_frame_delay`.
- Off-thread work operates on a snapshot taken on the UI thread and never touches a live `document` or `index_item`.
- A `document` belongs to one view's event sink. The Output pane's document and view have their own, because routing their changes to the document pane would corrupt that pane's wrap cache.
- Comments explain *why*, in one line. Do not narrate what the next line does.

## Where things live

| Area | Files |
|---|---|
| Platform, drawing and shared UI | the separate **libdesktop** repository: `libdesktop/core.h`, `platform.h`, `render.h` and `ui.h` and their libraries — windowing, drawing, files, settings, clipboard, spell check, tasks and child processes, the widgets, the text buffer, the syntax highlighters and the document, Markdown, CSV, hex and list views |
| Application | `app.h`, `app.cpp` (main window, panes, splitters, document index, search, session, dd integration, the Output pane), `app_state.h` (state and testable logic), `app_views.cpp` (the policy shared views ask for: message bar, Escape, zoom, popup menu) |
| Commands | `commands.h`, `commands.cpp` (`command_def` and lookup), `app_commands.cpp` (the command table and menu builder) |
| Text model | `document.h`, `document.cpp` (a `ui::text_buffer` with a path, encoding and line endings: load, save, JSON reformat, sort), `document_syntax.cpp` (which highlighter a document gets) |
| Document views | `view_base.h`, `view_text.h`, `view_doc.h`, `view_doc_edit.h`, `view_doc_readonly.h`, `view_doc_markdown.h`, `view_doc_csv.h` and `view_doc_hex.h` name the shared `ui` views locally; `view_output.h` (the Output pane and its line colouring) is dde's own |
| Panel views | `view_list.h` wraps the shared `ui::list_view`; `view_list_files.h` and `view_list_search.h` are dde's own and hang an `index_item` or a search hit off each row's `data` |
| dd integration | `dd_project.h`/`.cpp` (what a `dd.psd1` offers, the probe that reads it, and the one dd command line), `tool_runner.h`/`.cpp` (one child process at a time, with a queue), `tool_output.h`/`.cpp` (MSVC, clang, CMake, Ninja, CTest and PowerShell diagnostics) |
| C/C++ navigation | `cpp_lex.h`/`.cpp` (tokens), `cpp_index.h`/`.cpp` (declarations and the symbol table) |
| Widgets | `ui.h` (application helpers around the shared `ui` types) |
| Utilities | `util.h`/`util.cpp` (string ops), `json.h`/`json.cpp` (JSON reader), `calc.h` (expression parser for Calculate Selection), `gitignore.h` (index filtering) |
| Tests | `test.h` (assertions and runner), `test_process.h` (fake processes), `tests.cpp`, `tests_cpp.cpp` (lexer, parser, tool runner and output suites) |
| Build | `CMakeLists.txt` (declares the app with `libdesktop_add_app()` and registers `/test` with CTest), `CMakePresets.json`, `dd.psd1`, `dd.ps1` and `.dd/` (the vendored driver), `pch.h`, `targetver.h` |

## Adding a command

Add one entry to the table in `app_state::make_commands` (`app_commands.cpp`): description, menu text, `command_id`, accelerator, optional enabled and checked predicates, and the lambda. The table is fenced from clang-format, so lay the entry out like its neighbours. That entry drives the menu item, its enable and check state, `ui::dispatch_accelerator` and the generated About document. Do not add a parallel dispatch table, and do not also handle the key in a view — `ui::dispatch_accelerator` consumes it first, so the view's branch would be dead code. A second binding goes in `accel_alt`. Every binding is unique, and `should keep every binding unique` holds the table to it.

The dd menu's items that come from `dd.psd1` — a target, a test label, a project command — are generated by `build_dd_menu` and take no key. Everything with a key is a table entry.

The shared `ui` views handle Ctrl+A/C/X/V themselves, because an application with no menu behind a view would otherwise have no way to copy. Here the accelerator wins, so the view's branch is unreachable; do not "fix" that by removing the binding.

Global accelerators fire regardless of focus, so a command that acts on "the selection" must decide what the focused pane means — the editor, the file list, the Output pane, or an inline edit box.

## Command-line modes

`/test`, `/spell:<word>` and `/dd[:<folder>]` print to stdout without a window and write no configuration ([docs/design.md](docs/design.md#command-line)). The executable is a GUI program, so PowerShell waits for it, and shows its output, only when the output is piped: `& .\exe\dde-64d.exe /test | Out-String`.

`/dd` runs the real `dd.psd1` probe through `pwsh`, which unit tests cannot: they have no message loop to deliver its callbacks, and they must never start PowerShell. Run it after changing discovery, the runner or libdesktop's child-process code. In unit tests, give `app_state::spawn` (or `tools::runner::spawn`) a fake, as `test_process.h` does, and answer questions through `app_state::ask_user`.

## dd integration

The platform layer only starts processes and moves bytes: `platform::spawn_child_process` creates the pipes, runs a reader thread per pipe and delivers whole lines on the UI thread. It knows nothing of dd, diagnostics or the Output pane; keep it that way.

`dd_project` is the only place that knows dd's interface. Discovery reads `dd.psd1` as data, with `Import-PowerShellDataFile`; nothing runs the project's `dd.ps1` or `.dd` scripts because a folder was opened. Every dd command is an argument array built by `dd::command_line`, with the root as its working directory — never an assembled shell string, never an execution-policy override. [docs/design.md](docs/design.md#dd-integration) has the rest.

## The vendored dd

`dd.ps1` and `.dd/` are [dd](https://github.com/ZacWalk/dd), unmodified, at the revision whose file hashes [docs/dd-upstream.json](docs/dd-upstream.json) records; never edit them. Project settings live in `dd.psd1`; libdesktop's pin is the `GIT_TAG` in `CMakeLists.txt` (`dependencies.owner = 'application'`). dd's [two modes](https://github.com/ZacWalk/dd/blob/main/docs/mcp.md#two-modes) — CLI, and MCP for an MCP client — and its [commands](https://github.com/ZacWalk/dd/blob/main/docs/cli.md#commands) are documented in its own repository.
