# C++ tools

This document describes the planned C++ project and code tools for dde: the
small set of operations used every day, built without adding a dependency to the
editor and without turning it into a build system — dd is the build system.

The current editor implements lexical Go to Definition (`F12`), Switch
Header/Source (`Ctrl+F12`), and Back/Forward (`Alt+Left` / `Alt+Right`).
The dd menu, the Output pane and stepping through build diagnostics with `F8`
are implemented too; see [design.md](design.md#dd-integration).
Find References, symbol pickers and refactoring commands below remain planned;
their listed bindings do not mean those commands are available yet.
See [design.md](design.md#c-navigation) for the implemented navigation behavior.

## Decisions

1. **Nothing is linked or shipped but dde.** All semantic understanding
   of C++ comes from dde's own lexer and declaration parser, running
   in-process. No clangd, no Language Server Protocol, no libclang, no
   tree-sitter, no regular-expression engine.
2. **All understanding of tool results comes from in-process parsers** over a
   child process's output stream. Compiler, CMake, Ninja and CTest output is
   turned into structured diagnostics here.
3. **External programs are run, never linked.** dd, and the CMake and
   compiler it drives, are the user's toolchain, run as child processes through
   `pf::spawn_child_process`. Every one of them is optional.
4. **CMake is a source of facts, not a requirement.** When a build directory
   exists, its
   [File API](https://cmake.org/cmake/help/latest/manual/cmake-file-api.7.html)
   reply supplies targets and include directories, parsed with the existing JSON
   DOM. With no CMake, the index works from the file tree alone.
5. **Nothing runs because a folder was opened** except the index, which reads
   bytes the folder indexer is already reading, and the read of `dd.psd1`,
   which runs PowerShell's data-file reader and never the project's own code.
   Configuring, building, running and rewriting files all require an explicit
   command.
6. **Every operation is a function returning a value.** The menu and the
   command line are two front ends over one service. There is no
   second dispatcher and no operation that exists only as a menu handler.

## What this can and cannot do

The index resolves names *lexically*, with scope heuristics. It is not a
compiler front end and will never be one. State the consequences plainly,
because every later bug report is one of them:

- overloads resolve to a **set**, not to a single symbol;
- template-dependent names — anything reached through `T::`, or through a
  dependent base — are unresolvable;
- argument-dependent lookup, `using` declarations and namespace aliases are
  approximated by scope proximity;
- macros that construct declarations produce no symbol unless configured;
- code generated during the build is invisible until it exists on disk.

What is gained is not a consolation prize:

- it indexes code that does not compile, does not configure, and is half-typed;
- there is no server to start, so the first jump is instant;
- the index costs megabytes, not gigabytes, and no separate process;
- it reads the same bytes the folder index already reads, on the same pass;
- diagnostics come from the project's real compiler, so they are exactly the
  errors the build produces rather than a second opinion.

Where a result is uncertain the UI says so and offers the candidates. A picker
listing three declarations of `size()` is more useful than a confident jump to
the wrong one.

## Workspace model

dde has one open root folder, so that folder is the workspace. Opening a
different root replaces the model and stops any project-specific process.

The application owns one `cpp_workspace`:

- what the root's `dd.psd1` declares: targets, test labels and project commands;
- CMake presets, configured build directories, targets and include directories;
- the chosen configuration and test label;
- the symbol index and the file table it is keyed by;
- the last tool run and its parsed diagnostics.

Discovery and indexing run off the UI thread and publish immutable snapshots
through `pf::run_ui`. They must not retain pointers to live `document` or
`index_item` objects.

## Menus

The **dd** menu, between **Navigate** and **Help**, is implemented: its static items
are command-table entries, and what comes from `dd.psd1` — targets, test labels,
project commands — is generated in a menu-id range of its own whenever the
manifest is read, with no accelerators. [design.md](design.md#the-menu-and-its-keys)
describes it. What remains planned is navigation, refactoring and **Check
Current File**:

| Navigate | Binding |
|---|---|
| Go to Definition | `F12` |
| Find References | `Shift+F12` |
| Go to Symbol in Workspace | to be chosen: `Ctrl+T` is Help ▸ Run Tests |
| Go to Symbol in File | `Ctrl+Shift+O` |
| Switch Header/Source | `Ctrl+F12` |
| Back / Forward | `Alt+Left` / `Alt+Right` |

| Refactor | Binding |
|---|---|
| Rename Symbol | `F2` while the editor has focus |
| Extract Variable | none |
| Extract Lambda | none |

`F2` remains file rename while the file list has focus. That is one
focus-aware command, not two competing key handlers.

## Tool runner

`tools::runner` is the one execution path, and dd is what it runs: every
command is `pwsh -NoProfile -NonInteractive -File <root>\dd.ps1 <verb> <args>
--non-interactive`, built only by `dd::command_line`. A command is an
executable, an argument array, a working directory and a display name.
Arguments go straight to `pf::spawn_child_process`; no shell command string is
assembled, and `-ExecutionPolicy` is never passed.

A run produces a value, not a side effect:

```
tools::result { name, working_dir, started, cancelled, exit_code, elapsed_ms, output, dropped_lines,
                diagnostics, failed_targets }
```

The Output pane is one rendering of that value, line by line as it arrives;
the message bar's summary is another. Nothing may exist only inside the pane.

Runs are queued, not refused: a second request waits its turn. The queue is
bounded; a full queue is an error the caller can see. Cancellation applies to a
queue entry by id, whether it has started or not, and Stop drops everything
waiting.

While a run is in flight:

- stdout and stderr are collected in arrival order, with stderr identified;
- the message bar shows the newest output line and Ninja's progress;
- output is bounded in memory, keeping the beginning and the end;
- diagnostics are parsed incrementally, so `F8` works before the run ends.

Stopping must terminate the process tree, not just the immediate PowerShell
process. `pf::child_process` cannot yet — `terminate` ends only the process it
started, and `pf::process_spawn`, which owns a job object, reports no exit code —
so the capability and its test go into **libdesktop** first, then this
repository's pin moves. No process handles or job-object details belong here.

Modified files are not saved automatically: if any document is dirty, dde asks
to save, discard or cancel, and the command runs with the workspace root as its
working directory.

### `dd.psd1` discovery

Discovery reads the manifest and never runs a script: `pwsh` runs
`Import-PowerShellDataFile -LiteralPath ./dd.psd1 | ConvertTo-Json` from the root,
and `dd::parse_manifest` turns the JSON into a value. Dynamic values, and command
names computed by running project code, are deliberately not discovered.
[design.md](design.md#discovery-is-data-only) has the details.

## Output parsers

One `tool_output.cpp`, table-driven, hand-written matchers over `string_view`.
No `std::regex` — it is slow, heavy and unnecessary for line shapes this
regular. Each parser is a small function returning an optional diagnostic.

Formats to recognise:

- **MSVC** — `path(line,col): error C2065: text`, `path(line): warning C4996:`,
  `fatal error C1083`, and linker `LNK2019` with no file or line. The
  `see declaration of 'x'` and `while compiling class template member function`
  lines are continuations belonging to the diagnostic above them.
- **clang / gcc** — `path:line:col: severity: text`, `note:` continuations, and
  `In file included from …` chains.
- **Ninja** — the `[123/456]` progress prefix drives the message bar; `FAILED:`
  marks where a failing command's output begins.
- **CMake** — `CMake Error at path:line (command):` followed by an indented
  block. `-- ` status lines are not diagnostics.
- **CTest** — `Test #12: name .... Passed 0.03 sec`, the
  `The following tests FAILED:` summary, and the `--output-on-failure` body.
- **PowerShell** — multi-line error records.

Two invariants. A diagnostic owns its continuation lines as one record. A line
matching nothing is retained verbatim — **never drop a line**.

Strip ANSI CSI escape sequences before matching, and pass `NO_COLOR=1` in the
child environment: `pwsh` and modern compilers colour their output even when it
is redirected. Doing only one of the two leaves escape bytes in file paths.

dd streams whatever the tools beneath it print, so the parsers run over the
merged stream regardless of which command produced it. There is one parser
stack, not one per command.

This is pure string-to-struct code, so its test coverage should be close to
total and costs nothing: every format above gets fixture lines in `tests.cpp`.

## Diagnostics

Errors come from the compiler the project already uses, at two scales.

**Whole project** — parsed from the build output. These are ground truth.

**One file, on demand** — a syntax-only compile of the current translation unit
(`cl /Zs`, or `-fsyntax-only` for a clang-like driver) using that file's flags
from `compile_commands.json`. Sub-second for one file. Bound to an explicit
**Check Current File** command; never automatic, never on save, never on a
timer.

Both feed one diagnostic list. The Output pane shows it as the tools printed
it, and stepping through it with `F8` / `Shift+F8` already works as
[design.md](design.md#diagnostics) describes. A diagnostic keeps its compiler
code (`C2065`), because that is what the user searches for.

A dedicated left-hand panel is deliberately not the first version. It would make
the left pane a three-way choice rather than files-or-search, which is a change
to `view_mode` worth making only once the list has earned it.

## CMake project model

### Why `CMakeLists.txt` is not parsed

The CMake language has variables, functions, includes, conditionals,
toolchain-dependent branches, generator expressions and arbitrary process
execution. A text parser cannot determine the targets or the flags. The File API
exists to expose CMake's evaluated model as versioned JSON.

dde writes a client-owned query below the selected build directory:

```
.cmake/api/v1/query/client-dde/query.json
```

requesting `codemodel` v2 (projects, directories, targets, sources, compile
groups, includes, defines, artifacts), `cmakeFiles` v1, `toolchains` v1 and
`cache` v2. CMake reads the query during configure and writes replies under
`.cmake/api/v1/reply`. Always start from the newest reply index and follow its
`jsonFile` references; reply filenames are opaque. If a referenced file
disappears because CMake wrote a new reply concurrently, restart from the new
index.

### Presets

Read `CMakePresets.json` and `CMakeUserPresets.json` with the existing JSON
parser. Resolve `include` files, preset inheritance, `condition` objects, and
the `${...}`, `$env{...}` and `$penv{...}` macros needed to find `binaryDir`.
Hidden and condition-disabled presets supply inherited values but are otherwise
skipped. dde never invokes CMake itself — dd configures, builds and tests — so
presets are read only to find each configuration's `binaryDir`, where the reply
lands.

With no presets, use an existing build directory whose `CMakeCache.txt` names
the workspace as `CMAKE_HOME_DIRECTORY`. Otherwise the index works from the file
tree alone until dd has configured.

Reading existing File API replies during folder discovery is safe. Running CMake
is not — CMake files execute processes — so configure happens only when a dd
command asks for it.

### What the model is for

**Include directories for the index**, and later target selection; building and
testing are dd's. Later UI may group the file list by target, but the first
version leaves the physical folder tree unchanged: a source can belong to several
targets, so target membership must not become file ownership.

## The C++ index

Three pieces, in order of size.

### Lexer

A real C++ token scanner over a whole buffer: identifiers (bytes ≥ 0x80 are
identifier bytes), preprocessing numbers including digit separators, character
literals, string literals with every prefix, raw strings `R"tag(...)tag"`,
line and block comments, and punctuation. It tracks line numbers, marks the
first token on each line, and marks tokens inside a preprocessor directive —
including its backslash continuations — because the parser skips them wholesale.

A `//` comment continues onto the next line when the line ends in a backslash.
That is real C++ and a classic source of mis-scanning; it is a test case.

The per-line cookie scanner in `document_syntax.cpp` is **not** reused and must
not be. Its O(1) per-line invalidation is what keeps typing cheap in a large
file, and it cannot represent the multi-line constructs above. Two scanners, one
token vocabulary, and one line in the code saying why.

Tokens are slices of the buffer — offset and length, so no text is copied and a
token cannot span a line splice. An identifier broken across lines by a
backslash is therefore not recognised; that is an accepted limitation.

### Declaration parser

A brace, paren and bracket matcher with a scope stack: `namespace` →
`class` / `struct` / `union` / `enum class` → function. It emits a symbol for
each declaration at scope level and then **skips function bodies wholesale**,
recording only their byte range. No syntax tree, no type analysis, one small
stack, one pass, O(bytes).

That is the whole trick: almost all of C++'s difficulty lives inside function
bodies, and almost no navigation target does.

### Symbol table

```
symbol { name_ref u32, scope_ref u32, file_ref u32, offset u32, line u32, kind u8, flags u8 }
```

Twenty-four bytes, names interned in one table whose entries never move, keyed
by file so a reparse splices one file's range out and reinserts it. A
single-file reparse is fast enough on a debounced idle that incremental parsing
*within* a file is unnecessary; do not build it.

The qualifier belongs to the name directly after `::`, and to nothing else:
`std::vector<T> tokenize(...)` declares `cpp::tokenize`, not `cpp::std::tokenize`.
That one rule is what keeps out-of-line definitions (`void app_state::open()`)
and qualified return types apart.

### The preprocessor

**Index every branch of every `#if`**, treat macros as opaque, and record the
conditional nesting each symbol was found under. Evaluating conditionals
correctly needs include resolution and macro expansion — that is a compiler.
Indexing all branches is what tag-based tools have always done, and it is right
for navigation: a declaration in the branch you are not building is still a
declaration you want to reach. Mark such results rather than hiding them.

Keep one global macro table — name to replacement text — for exactly two
purposes: knowing that `FOO(x)` is a macro invocation and not a call, and
expanding a short user-configured list of declaration macros. Nothing else.

`#include "x.h"` resolves against the including file's directory, then the
CMake model's include directories when present, then the root.

### References are not stored

Storing every identifier occurrence would cost tens of megabytes and dominate
the process. Find References is instead the **existing folder search** — already
off the UI thread, already `.gitignore`-filtered — with two additions: the lexer
rejects hits inside comments, strings and disabled-looking regions, and each
surviving hit is scored against the symbol's scope. Zero index memory, and it
reuses the machinery in `app.cpp` instead of growing a second copy.

If that proves too slow on a large tree, add a per-file bitset of name hashes to
skip files that cannot contain the name. Measure before building it.

## Navigation

At the caret, resolve the identifier against the symbol table, ranked by scope
proximity: definitions before declarations, the current file before others, a
type before a variable. One confident result jumps; several are **cycled** —
pressing the key again on the same name offers the next, with the message bar
saying which one of how many. That is the picker the plan called for, minus the
popup: the platform layer has `screen_to_client` but no `client_to_screen`, so
placing a menu at the caret would mean a change in another repository for a
mouse-only affordance. Revisit it when something else needs that call.

Locations are canonicalised before opening. Workspace files open through the
normal folder-index and document path; files outside the root open read-only.

Every successful jump pushes the current path and caret onto a back stack and
clears the forward stack; `Alt+Left` and `Alt+Right` walk the two. Applying
history uses `load_doc`'s completion callback, because the destination may still
be loading.

Find References populates the search result view from located hits rather than a
text query, grouped **confident** and **possible**, and navigating one uses the
same asynchronous selection path as ordinary search.

## Refactoring

### Rename

Rename is **preview-first, always**. Resolve the symbol, collect every candidate
site, group by confidence with the confident group checked, and apply only what
the user confirms. This is the honest design for a lexical index — and it is
arguably better than a silent partial rename, because the sites it is unsure
about are exactly the ones worth looking at.

Before changing anything, validate the whole edit:

- every target is a regular text file inside the workspace;
- no target is truncated by the 2 MB cap or read-only — either aborts the whole
  rename;
- affected documents are pinned so the document cache cannot evict one
  mid-transaction;
- ranges are valid, non-overlapping and on UTF-8 boundaries.

Load every affected document first, then apply edits from the end of each file
inside one `undo_group` per document. All-or-nothing; never saves. A failed
preflight leaves every file untouched. Successful edits stay dirty in memory and
show red in the file list.

### Extract Variable

`auto name = <expr>;` needs no type information at all. Validate that the
selection is brace, paren and bracket balanced, contains no `;` or `return`, and
lies inside a function body the parser recorded. Insert the declaration before
the statement containing the selection and replace the selection with the name.

### Extract Lambda

A full extract-to-function needs parameter types, which this index does not
have. `auto name = [&] { ... };` placed immediately before the selection
captures everything by reference and writes no types. Ship that; promoting the
lambda to a free function stays manual, and the documentation says so rather
than pretending otherwise.

## Failure handling

Missing tools are ordinary feature states:

- no `pwsh` — the dd menu says it was not found and stays disabled; the index
  still works;
- no `dd.ps1` — the dd menu says so; editing, search and the index still work;
- no File API reply — wait for dd to configure rather than guess the project;
- no compilation database — the index resolves includes file-relative and says
  results may be incomplete;
- crashed tool — keep its output and exit status, clear the busy state, let the
  next queue entry start a fresh process.

Malformed output is reported in the Output pane or the message bar, and must
never terminate the editor. Index progress is a status
message, not a modal dialog.

## Budgets and acceptance tests

Numbers, so the design can be falsified:

- index throughput ≥ 50 MB/s on the worker thread, with no allocation in the
  lexer's inner loop — **measured at 80 MB/s** over this repository's own
  sources, so the whole tree is indexed in the time one file takes to load;
- ≤ 40 bytes resident per symbol, arena-allocated, no per-symbol heap node;
- single-file reparse under 5 ms for 5,000 lines;
- full index of this repository and libdesktop well under one second;
- no per-keystroke work beyond marking one file dirty.

One checked-in acceptance test decides whether the approach works at all: index
this repository, then assert that a fixed list of symbols resolves to the right
scope — a class, an out-of-line member definition, a free function in a
namespace, a method declared inside a class body, a struct. It also holds a
throughput floor, low enough for a loaded machine and high enough to catch a
collapse. That test is what stops the parser rotting, and it came before any UI
was built on the index.

## Delivery order

1. **Lexer.** Token stream and its fixture tests. Nothing depends on anything.
2. **Output parsers, tool runner, dd discovery, the Output pane.** Useful on its
   own and independent of the index. Done.
3. **Declaration parser, symbol table, folder-index integration**, and the
   acceptance test above. If that test disappoints, stop here having lost
   nothing.
4. **Navigation.** Definition with picker, file outline, workspace symbols,
   switch header/source, back/forward history.
5. **CMake model.** Presets, File API, targets, include directories.
6. **Find References** over the existing search, with confidence grouping.
7. **Preview-first rename**, then Extract Variable and Extract Lambda.
8. **Check Current File** diagnostics.

Every behaviour belongs in `tests.cpp`, and process launch stays injectable so
unit tests never start PowerShell, CMake or a compiler. Add command-line
diagnostics only where `/test`'s missing message loop makes a real process check
necessary. After any libdesktop change, run its suite, push it, and bump
`GIT_TAG` here before relying on the new API.

## Not in the first version

- completion, signature help, hover, or semantic colouring — a lexical
  completion list that is often wrong is worse than none, and member completion
  needs the local declarations inside the function bodies the parser skips;
- debugging and breakpoints;
- package-manager integration;
- parsing other build systems into CMake-like targets;
- automatic build, configure, save, index-on-open beyond the folder index, or
  refactor without confirmation.
