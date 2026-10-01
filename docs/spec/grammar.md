# MVP Grammar

The normative grammar lives in [`grammar.ebnf`](grammar.ebnf). This
document keeps what EBNF cannot say: lexical rules, precedence notes,
and the disambiguations. Changing the grammar means changing both
files in the same commit. This grammar covers MVP only; reserved forms
are rejected with guidance diagnostics.

## Lexical notes

- `..`, `..=`, `..<` are single tokens and open a range; either
  endpoint may be absent, and an absent one is unbounded (`1..`,
  `..<3`, `..`). A range that names an end says how it is bound:
  `..=` includes the end endpoint and `..<` excludes it, and those
  two are the only operators that may be followed by an endpoint, so
  a bare `..` before one is an error. A start endpoint is always
  included; no spelling excludes it. A newline after the operator
  terminates the statement, so `n := 0..` is a complete value. After
  an integer literal, `..` starts a range operator, never a float
  fraction (`1..<2` is `1 ..< 2`); `1. < 2` keeps float-then-compare.
- Nesting is bounded at 256 levels, counted on expressions, types,
  blocks, patterns, and items alike. Exceeding it is a diagnostic, not
  an error the program can observe: the parser, checker, and lowerer
  all stop descending and report `E3004`/`E4050`/`E5005`. The limit
  keeps a pathologically nested file from exhausting the stack, and it
  is a language property rather than an implementation detail, so every
  stage applies the same number.
- Newlines are significant: outside brackets, a newline is lexed as
  `;` when the preceding token ends a statement (identifiers,
  literals, `)`, `]`, `}`, `break`, `continue`, `ret`) unless the
  next token continues the construct (`else`, `,`, closers, `.`, or
  end of file). A line therefore continues only in operator-led,
  bracket-open, or continuation positions.
- Block openers stay on their header line: the `{` of `if`, `while`,
  `match`, `fn`, `impl`, and `else` MUST NOT start on a following
  line (Go-style). `} else {` stays on one line.
- Block comments nest. `///` attaches to the following item.
- Integer literals accept `_` separators between digits. Suffixes
  (`42i32`, `1.5f64`) select the type; unsuffixed integers default
  to `i32`, floats to `f64`.
- String escapes: `\n \t \r \\ \" \0 \u{HEX}`. `'x'` character
  syntax exists but resolves only with core (see `types.md`).
- Longest-match lexing applies throughout.

## Items

Methods take explicit receivers: `self`, `&self`, `&mut self`.
`Self` denotes the implementing type inside `impl` blocks, and the
spec's own type inside `spec` declarations. An omitted
return type means `()`; only free functions take type parameters.

A `spec` declares a named capability as `;`-terminated method
signatures, with no bodies. `impl S for T` implements every
declared method for one type; a missing or extra method, or a
signature that differs after substituting the target for `Self`,
is an error where the impl is written. The tree holds at most one
impl of a spec for a type: a second overlapping impl conflicts,
and two in-scope specs providing one method name make a call
ambiguous. A method call tries inherent impls first, then spec
impls whose spec is in scope (declared alongside, imported, or in
the prelude).

## Types

`()` is the unit type. `!` is the never type and coerces to any type.
Tuple types are structural and concrete (no polymorphism in MVP).
A path takes type arguments for generic enums and structs only, and a
closing `>>` splits into two `>` (dangling halves error).

## Patterns (shared by declarations and `match`)

A `-` binds only to an integer or float literal, so `-1` is one
pattern and `x - 1` never parses as a pattern. The sign is not part of
the literal's spelling; it is recorded separately, so the magnitude
parses identically either way.

Declaration left-hand sides use this grammar with `:=`
(`(c, _) := ...`, `_ := ...`). Range patterns are deferred.

## Expressions (lowest to highest precedence)

- `?` binds tighter than all binary operators.
- `%`, `&`, `|`, `^`, `<<`, and `>>` are integer-only; applying one to
  a float is rejected. Floats admit `+ - * /` and the comparisons.
- Unary `*` dereferences a reference into the place it names.
  Assignment through it needs a `&mut` reference.
- A turbofish supplies explicit type arguments to a generic call;
  without one, the parameters bind from the argument types.
- Array indexing is builtin with panic-on-out-of-bounds semantics.
- Calls to `panic(...)` diverge with type `!`.
- Closures do not exist; `||` is logical-or only.
- In statement position `Path {` opens a struct expression. After
  `if`/`while`/`match` conditions, `for` heads, and `else`, `{` always
  opens a block: parenthesize expressions containing struct literals.
  A `for` head additionally never reads a block as a range end, so
  `for i in 0..` takes the open range as its head.
- `match` scrutinees and `if`/`while` conditions never parse a
  struct literal directly (the `{` belongs to the body); this keeps
  `match x {` and `if c {` unambiguous without lookahead. Parentheses
  lift the ban, so `if (Foo { x: 1 }).x > 0` parses.

## Statements and blocks

- Newlines terminate statements; `;` separates multiple statements on
  one line only (a trailing `;` is an allowed no-op). Stray `;` are
  skipped in item, block, and arm lists; `match` arms accept runs of
  `,` and `;` as separators.
- A block's value is its last expression; `{}` evaluates to `()`.
- `if cond block (else block)?` and `if pattern := expr block
  (else block)?`; `while` mirrors `if` (both accept
  pattern-declarations).
- `match scrutinee "{" (pattern "=>" expr ",")* "}"`
- `loop block`, `while cond block`, `for pattern "in" expr block`;
  `break`, `continue` (unlabeled); loops evaluate to `()`. The `for`
  rule lives in `control.md`.
- `ret expr?` returns early from the enclosing function.

## Modules and paths

Module declarations come exclusively from `alcy.toml` `[modules]`.
Files not included in `include` and unreachable from declared modules
warn (see `modules.md`).
