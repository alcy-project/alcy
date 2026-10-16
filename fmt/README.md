# fmt

The source formatter. It reads alcy source and writes it back in one
canonical layout.

The formatter is a library, not a compiler stage: the compiler never
depends on it, and it owns only the concrete syntax tree it builds while
formatting. The command line reaches it through `alcy fmt`.

## Shape

- `lex`: a lossless token stream. The compiler's lexer already knows
  every token; this layer adds back the comments it treats as trivia and
  keeps the newline-inserted semicolons so the layout can see where a
  statement ended.
- `cst`: a concrete syntax tree built from that stream. It nests
  delimiters only, so an input with unbalanced or unexpected tokens still
  becomes a tree; nothing is dropped, so the tree is a faithful record of
  what was read.
- `doc`: a small layout algebra (`text`, `line`, `group`, `nest`)
  and a renderer that chooses between a flat and a broken rendering
  against a line budget.
- `format`: the public entry point. It turns a token stream into a
  document and renders it.

## Configuration

The layout parameters are injected as a `FormatConfig`. Nothing reads a
file, an environment variable, or a terminal: the tool always passes
`default_format_config()`, so one input formats the same way on every
machine. A host that wants another width passes its own config; the
formatter holds none.

## Properties

- **Idempotent**: formatting formatted output changes nothing.
- **Tolerant**: any byte string becomes a tree, and a construct that does
  not parse is laid out as it was read rather than dropped. No input
  crashes the formatter.
