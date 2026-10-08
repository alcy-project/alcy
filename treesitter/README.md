# The alcy grammar for tree-sitter

The grammar here is a second reading of the language. The first is the
compiler; the normative statement of what is being read is
[`docs/spec/grammar.ebnf`](../docs/spec/grammar.ebnf), with what EBNF
cannot say in [`docs/spec/grammar.md`](../docs/spec/grammar.md).
`grammar.js` follows both production for production.

## What it is checked against

Three oracles, in `tools/check_treesitter.py`:

- **The generated sources are current.** `tree-sitter generate` rewrites
  `src/` and `git diff` says whether the committed copies match, so a
  change to `grammar.js` cannot land without the tables it produces.
- **The corpus passes.** Every area of the specification has a case under
  `test/corpus/`, and one more file for the cases where the grammar has to
  choose between two readings and the choice is the whole point.
- **The compiler agrees.** Every `.al` file the compiler accepts parses
  here without an `ERROR` or a `MISSING` node. That direction is the one
  that matters and it is checked on every source in the repository, which
  makes it a relation rather than an example.

`test/parse_errors.txt` holds the other direction: cases the compiler
rejects for a failure to parse, which this grammar must reject too. It is
empty, and the file says why.

## Two rules no regular expression can express

Both live in `src/scanner.c`.

**The brace that opens a block.** The specification requires the `{` of
`if`, `while`, `match`, `loop`, `for`, `fn`, `impl`, `struct`, `enum`,
`spec`, `extern`, `comp`, `unsafe`, and `else` to stay on the line of the
header it belongs to. That brace is `_block_lbrace`, and the scanner
produces it only when no newline stands between the header and the brace,
and asks for it before any other token. So in a header position it wins
over the plain `{` a struct literal would use, which keeps `if c { }` a
block rather than a struct literal on `c` and makes parentheses the way to
lift that ban, as the specification says they are.

A block used as an expression in its own right has no header, so its brace
is an ordinary one and `{ ... }` on a line of its own is a block-valued
statement.

**The `;` at the end of a line.** The rule that a newline ends a
statement is split in two, and only half of it is here. Whether a newline
may end a statement at all is decided by where `_newline` appears in
`grammar.js`; whether the next token continues the construct is decided by
the scanner. That is why there is no second copy of the lexer's list of
statement-ending tokens to forget to update when the language changes.

Block comments nest, which is the third thing the scanner does, because no
regular expression can count depth.

## Where this grammar is more permissive

A parse-only front end has no passes to reach the diagnostics the compiler
reaches later, so it reads code the compiler refuses. That is allowed and
is the safe direction: it can accept too much, never too little. What it
does not do is silently disagree about the shape of a program the compiler
accepts, which is what the differential check is for.

Three places read wider than the compiler, and each is pinned by a corpus
case:

- **A bare `..` followed by an operand.** `1..3` reads as an open-ended
  range followed by a statement holding `3`, which is a legal reading of
  the two lines. Only the diagnostic knows the range was meant to have an
  end. The corpus case is named for the reading it records.
- **A chained comparison.** `a < b < c` parses. The compiler takes one
  comparison and no more.
- **The deferred integer widths.** `i128` and `u128` parse as primitive
  types. The compiler knows the tokens too, and rejects the type itself,
  which is a diagnostic no parse-only front end has a pass for.

One place reads wider by design: a parenthesized expression is a node.
`(a)` wraps rather than yielding its contents unchanged, and the braces it
lifts are the reason it is worth having.

Two places read narrower:

- **A float needs a digit after the point.** `1..<2` must not reach for the
  second `.`, and asking for one digit is what keeps it from doing so. The
  cost is that `1.` is an error here and a float in the specification.
- **A block opener on a following line.** The specification forbids it, so
  `if c` and a `{` on the next line is an `ERROR` node, as it is a
  diagnostic in the compiler.

## Reserved words

`async`, `await`, `union`, `register`, `where`, and `dyn` are
identifiers here. They are not identifiers in the compiler: the lexer
gives each its own token kind and the parser reads it past with a
diagnostic (`parser.cc`, "skip_insignificant"). A tree built here and a
tree the compiler builds therefore agree, which is the point. An editor
that wants to flag them has no node to match, and that is the one thing
lost.

## Nesting

The compiler bounds nesting at 256 levels on expressions, types, blocks,
patterns, and items alike, and reports it rather than exhausting the stack.
Nothing bounds it here. The limit is a property of the language and a
policy of the implementation, and a grammar that imposed it would reject
files the compiler rejects for a reason an editor has no use for.

## Generated sources

`src/` is committed, apart from `src/scanner.c`, so that a consumer links
the parser without running the generator. Everything else in it comes
from `grammar.js`, and the drift check is what keeps the two in step.

## Using it in an editor

`queries/` is nested under `alcy/` rather than sitting at the top of the
directory. The tree-sitter CLI reads the paths named in `tree-sitter.json`,
so it does not care, and every editor that looks for
`queries/<language>/` finds them without being told where they are. A flat
`queries/highlights.scm` would have needed a path handed to each editor by
hand.

Neovim 0.11 and later has treesitter built in, so nothing is installed:

```lua
-- Build the parser first: tree-sitter build -o parser/alcy.so
vim.treesitter.language.add("alcy", { path = "<path>/parser/alcy.so" })
vim.filetype.add({ extension = { al = "alcy" } })
vim.opt.runtimepath:append("<path>")   -- for the queries
```

