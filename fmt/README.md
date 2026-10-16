# fmt

The source formatter. It reads alcy source and writes it back in one
canonical layout.

The formatter is a library, not a compiler stage: the compiler never
depends on it, and it leans on the compiler's lexer alone. The command
line reaches it through `alcy fmt`, which writes the files it is
handed (standard input to standard output when it is handed none) and
reports rather than writes with `--check`.

## Shape

- `lex`: a lossless token stream. The compiler's lexer already reads
  every token; this layer adds back the comments it treats as trivia
  and keeps the newline-inserted semicolons marked, so the layout can
  tell a spelled separator from a line break.
- `cst`: a concrete syntax tree built from that stream. It nests
  delimiters only, so an input with unbalanced or unexpected tokens
  still becomes a tree; nothing is dropped. A brace is classified by
  the header in front of it, because a run of statements, a list of
  members, and a struct literal lay out differently.
- `doc`: a small layout algebra - text, line, group, nest - and a
  renderer that chooses between a flat and a broken rendering for each
  group against the width it was given.
- `format`: the public entry point, `format(source, config)`.

## Layout

- An expression block (`unsafe { ... }`, `comp { ... }`) and a struct
  literal read on one line while they fit; function and control-flow
  bodies, declarations, and match bodies take a line each, which is
  how the library's own sources read.
- A spelled-out semicolon survives wherever the newline that would
  replace it reads differently: a spec or extern member, a signature
  the lexer does not end a statement after, and a body's last
  statement, which keeps its statement status rather than becoming the
  block's value.
- Comments keep their line and their attachment: one that ends a line
  rides after the code it follows, one on its own line rides before the
  code it introduces.

## Configuration

The layout parameters are injected as a `FormatConfig`. Nothing reads a
file, an environment variable, or a terminal: the tool always passes
`default_format_config()`, so one input lays out the same way on every
machine. A host that wants another width passes its own; the formatter
holds none.

## Properties

- **Idempotent**: formatting formatted output changes nothing.
- **Tolerant**: any byte string becomes a tree, and a construct that
  does not parse is laid out as it was read rather than dropped. No
  input crashes the formatter.

`tools/check_fmt.py` asks both of every `.al` file in the repository,
along with the layout the tree already has. See
`docs/adr/0081-a-formatter-over-a-concrete-syntax-tree.md` for the
decision the shape follows.
