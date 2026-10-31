# ADR-0081: The formatter reads a concrete syntax tree

- Subject: the toolchain
- Status: Accepted
- Date: 2026-10-16

## Context

The bootstrap vision names a formatter among the tools the language
ships. The compiler already has a parser, but its tree is abstract by
design: comments are trivia it skips, the semicolon a newline stands
for is gone by the time it sees a statement, and a construct that
fails to parse leaves the tree altogether. A formatter needs the
opposite of all three — it writes back what was read, so the bytes it
emits must account for every comment and for the statement boundaries
the source's own line endings chose — and it must lay out a file that
does not parse, because the file a formatter is most wanted for is the
one being edited.

Two further constraints come from the tool rather than the tree. A
formatter's output must be a fixed point: a second pass that moves
anything is a review nobody asked for. And the layout parameters — the
line length above all — are the host's business: the tool runs in
editors and pipelines with widths of their own, so they travel as
arguments rather than living in the tool.

## Decision

**The formatter is a module of the toolchain tree under `src/fmt/`, it builds a concrete syntax
tree rather than reading the compiler's abstract one, and its layout
parameters are injected while it holds no configuration of its own.**

- The token layer reuses the compiler's lexer and adds back what it
  treats as trivia: comments return as lexemes, and the semicolons the
  lexer inserts at newlines are marked, so the tree can tell a spelled
  separator from a line break.
- The tree nests delimiters only. Every token keeps its place, an
  unclosed delimiter is a node without a closer rather than an error,
  and a stray closer is a leaf, so any byte string becomes a tree and
  nothing is dropped. Nesting is bounded at the compiler's own
  `MAX_NESTING`, past which an opener is read as a leaf.
- A brace is classified by the header in front of it — a match body,
  an item body, a block — because the layout of what a brace holds
  differs enough that one "brace" case would have to guess at print
  time. The classification reads the same header the grammar does.
- The layout parameters are a `FormatConfig` handed to `format`. The
  command passes `default_format_config()` and nothing else: no flag
  names a width, nothing reads a file or the environment, and a host
  wanting another width passes its own.
- Idempotence and parse tolerance are properties the gate asks of the
  whole corpus (`tools/check_fmt.py`): every `.al` file in the tree is
  what the formatter would write, formatting formatted output changes
  nothing, and the compiler's verdict about a file survives its
  formatting. The last clause is why a spelled-out semicolon survives
  where dropping it would read differently — a spec or extern member
  needs it, or a signature the lexer does not end a statement after
  needs it. A spelled-out semicolon is never load-bearing as a
  block's statement-vs-value switch: block value is the last
  expression by position, discarding a non-`()` value goes through
  `_ :=`, and a function body still needs an explicit `ret` on every
  path, so its tail expression is never the implicit return
  (ADR-0059).

The library is a module of the toolchain tree rather than a stage of
the compiler: no compiler target depends on it - the dependency
direction is the boundary, and gn check holds it - and it leans on
the lexer alone. The lossless front end is its own module, `src/cst`:
the tools' counterpart to `ast/`, one tree per kind of reader, held
apart so the linter that comes can read the same tree the formatter
does. Its headers take the flat `fmt/...` spelling the module's
directory gives them. The vendored formatting library owns a fixed set
of flat `fmt/<name>.h` spellings of its own, and the module's files
never reuse one of those names, so the two prefixes cannot meet.

## Consequences

A construct the grammar rejects is laid out by its tokens rather than
diagnosed, so a formatter run is never blocked by a parse error — and
never reports one; the compiler's diagnostics stay the compiler's.
Recovering the layout the way the source spelled it costs the tree the
trivia the abstract tree is glad to be rid of, which is the reason for
a second tree rather than a share of the first. The samples the
language shows were already in the formatter's canonical layout when
it landed, so what it reformatted was the cases and the library, whose
hand layout was inconsistent in exactly the ways a formatter exists to
settle.
