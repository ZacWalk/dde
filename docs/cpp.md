# The C/C++ index

Go to Definition, Switch Header/Source and Back/Forward run on dde's own index
of the folder's C and C++ sources: a lexer (`cpp_lex`) and a declaration-level
parser and symbol table (`cpp_index`). Nothing else is linked or run for it —
no compiler, language server, libclang or tree-sitter. Diagnostics come from
the project's own compiler, through dd ([design.md](design.md#dd-integration)).

## What it can and cannot do

Names resolve lexically. The index is not a compiler front end, and every
limitation follows from that:

- a name resolves to every declaration of that name in the folder, whatever its
  scope, so an overload set is a set of candidates, not one symbol;
- names reached through a template parameter (`T::`) or a dependent base,
  argument-dependent lookup, `using` declarations and namespace aliases are not
  resolved;
- a declaration produced by a macro expansion has no symbol;
- a file the folder index leaves out — gitignored build output, a fetched
  dependency — is not indexed, and nor is a file over 4 MB.

What that buys: code that does not compile, does not configure or is half-typed
still indexes; there is no server to start; and the index costs memory in
proportion to its symbols, in dde's own process. Where a name has several
candidates the message bar says so and `F12` steps through them.

## Lexer

`cpp_lex` scans a whole buffer into tokens: identifiers (bytes at or above 0x80
are identifier bytes), preprocessing numbers with digit separators, character
and string literals with every prefix, raw strings, comments and punctuation. It
records each token's line and byte column, marks the first token on each line,
and marks tokens inside a preprocessor directive, including its backslash
continuations. A `//` comment ending in a backslash swallows the next line, as
it does in C++.

Tokens are slices of the buffer, so no text is copied and a token never spans a
line splice; an identifier broken by one is not recognised.

The lexer is deliberately not libdesktop's per-line syntax highlighter. That
scanner's single carry cookie is what keeps typing cheap, and it cannot
represent a raw string, a spliced line or a continued directive. Keep the two
apart.

## Declaration parser

`cpp::parse_declarations` is a brace, paren and bracket matcher with a scope
stack — namespace, class, struct, union, enum, function. It emits a symbol for
each declaration at scope level and **skips function bodies whole**. There is no
syntax tree and no type analysis: one pass, linear in the bytes. Almost all of
C++'s difficulty lives inside function bodies, and almost no navigation target
does.

It indexes namespaces, classes, structs, unions, enums and their enumerators,
functions (free and member alike; the scope tells them apart), variables, type
aliases, concepts and macros.

A qualifier belongs to the name directly after `::` and to nothing else:
`std::vector<T> tokenize(...)` declares `cpp::tokenize`, not
`cpp::std::tokenize`. That one rule keeps out-of-line definitions
(`void app_state::open()`) and qualified return types apart.

### The preprocessor

Every branch of every `#if` is indexed and macros are opaque: evaluating
conditionals needs include resolution and macro expansion, which is a compiler.
A declaration in the branch you are not building is still one you want to
reach, so a symbol inside an `#else` or `#elif` is flagged rather than hidden,
and ranks below its sibling. A `#define` indexes the macro's name only.

## Symbol table

A `cpp::symbol` is an interned name and fully qualified scope, a file, the
name's byte offset, line and byte column, a kind and flags (definition,
alternative branch), in at most 32 bytes. Interned strings never move. Symbols
are kept per file, so a changed file is reparsed and its symbols replaced
without touching the rest, and a name-to-symbols map answers a lookup without
scanning.

The index is rebuilt off the UI thread after every folder refresh, from paths
taken on the UI thread, for `.h`, `.hpp`, `.hh`, `.hxx`, `.inl`, `.c`, `.cpp`,
`.cc`, `.cxx` and `.ixx` files. Sources are decoded to UTF-8, UTF-16 included,
so positions match document byte offsets. A generation number discards a
rebuild that a newer refresh or another root folder has overtaken. Only the UI
thread reads the published index. Every open source document is reparsed into
it when it is published and before every lookup, so a lookup sees unsaved edits,
and undo back to the saved text, rather than the file on disk.

## Navigation

`F12` takes the name at the caret and ranks its candidates: definitions before
declarations, then the file the lookup started from, then code outside an
`#else` or `#elif`, with variables last; ties go by path and position. It jumps
to the best. Pressing `F12` again at the destination offers the next candidate,
and the message bar says which one of how many — a picker without a popup. The
starting file keeps its preference until the caret moves, so jumping into
another file cannot reorder the rest.

`Ctrl+F12` switches between a header and its source, preferring the
counterpart in the same folder. `Alt+Left` and `Alt+Right` walk back and forward
through up to 64 visited locations, skipping deleted files and clamping
positions a file no longer has; both stacks reset when the root folder changes.
A jump into a document that is still loading is applied once it has loaded.

Right-clicking C/C++ text offers **Go to Definition** and **Switch
Header/Source** above the editing commands. Definition uses the clicked name,
even inside a selection, which Copy still keeps; a menu opened from the keyboard
uses the caret. These commands work only in the focused C/C++ text view,
read-only source included — not in the previews, the file list or the Output
pane.

## Tests

`tests_cpp.cpp` holds the lexer and parser suites. `should index this
repository` indexes dde's own `src/` and asserts that a fixed set of real
declarations — a class, an out-of-line member definition, a free function in a
namespace, a method declared in a class body, a struct — resolve to the right
scope. The runner reports elapsed time for information, not as a pass/fail
threshold: correctness must not depend on build configuration or machine load.
It is what keeps the parser honest; keep it passing.
