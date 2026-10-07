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
  an error the program can observe: the parser, the checker, and the
  lowerer all stop descending, each reporting the code of its own
  component. The limit keeps a pathologically nested file from
  exhausting the stack, and it is a language property rather than an
  implementation detail, so every stage applies the same number.
- Newlines are significant: outside brackets, a newline is lexed as
  `;` when the preceding token ends a statement (identifiers,
  literals, `)`, `]`, `}`, `break`, `continue`, `ret`) unless the
  next token continues the construct (`else`, `,`, closers, `.`, or
  end of file). A line therefore continues only in operator-led,
  bracket-open, or continuation positions.
- Block openers stay on their header line: the `{` of a block that
  belongs to a header — `if`, `else`, `while`, `for`, `loop`, `match`,
  `fn`, `impl`, `spec`, `struct`, `enum`, `extern`, `comp`, and
  `unsafe` — MUST NOT start on a following line (Go-style). A newline
  inside a comment does not move the line. `} else {` stays on one
  line.
- Block comments nest. `///` attaches to the following item.
- Integer literals accept `_` separators between digits. Suffixes
  (`42i32`, `1.5f64`) select the type; unsuffixed integers default
  to `i32`, floats to `f64`.
- A float literal may carry an exponent, with or without a fraction:
  `2e9`, `1.5e-3`, `6.02E23`. The exponent needs at least one digit, and
  its sign is optional.
- String escapes: `\n \t \r \\ \" \0 \u{HEX}`. `'x'` character
  syntax exists but resolves only with core (see `types.md`).
- Longest-match lexing applies throughout.

## Items

Methods take explicit receivers: `self`, `&self`, `&mut self`.
`Self` denotes the implementing type inside `impl` blocks, and the
spec's own type inside `spec` declarations. An omitted
return type means `()`; only free functions take type parameters.

A `spec` declares a named capability as `;`-terminated method
signatures, with no bodies. A spec may refine another: `spec Eq:
PartialEq` says every implementation of `Eq` also needs an
implementation of `PartialEq` for the same target, the super must
resolve to a spec in scope, and the chain must not cycle. Generic
super-specs are not supported yet. `impl S for T` implements every
declared method for one type; a missing or extra method, or a
signature that differs after substituting the target for `Self`,
is an error where the impl is written. The tree holds at most one
impl of a spec for a type: a second overlapping impl conflicts,
and two in-scope specs providing one method name make a call
ambiguous. A method call tries inherent impls first, then spec
impls whose spec is in scope (declared alongside, imported, or in
the prelude).

An `unsafe fn` declares that calling it is an operation that needs
the gate; a `comp fn` declares that calling it happens during
compilation only (see `comp.md`), and the two markers do not
combine yet. An unsafe function's body is not an unsafe context
implicitly, so every operation the gate covers names `unsafe { ... }`
where it happens, and the gate never suspends region, move, or drop
checking. See `ffi.md`.

An `extern "C" { ... }` block declares bodyless functions whose
names are the symbols the linker resolves. `"C"` is the only
convention accepted, calling a declaration needs `unsafe`, and the
initial ABI is scalars, raw pointers, and `()` as a return; see
`ffi.md`.

## Types

`()` is the unit type. `!` is the never type and coerces to any type.
Tuple types are structural and concrete (no polymorphism in MVP).
A path takes type arguments for generic enums and structs only, and a
closing `>>` splits into two `>` (dangling halves error). A function
type is structural: `(A, B) -> R`, with `()` and `(A)` as its no- and
one-parameter spellings; a bare `(A)` outside the arrow position is
the type it wraps. `*T` and `*mut T` are raw pointers: thin, Copy,
and outside the region system; `**T` lexes as the power token but a
type position reads the pair as two stars. See `ffi.md`.

## Patterns (shared by declarations and `match`)

A `-` binds only to an integer or float literal, so `-1` is one
pattern and `x - 1` never parses as a pattern. The sign is not part of
the literal's spelling; it is recorded separately, so the magnitude
parses identically either way.

Declaration left-hand sides use this grammar with `:=`
(`(c, _) := ...`, `_ := ...`). Range patterns are deferred.

## Expressions (lowest to highest precedence)

- `?` binds tighter than all binary operators.
- `?` is a postfix operator, so it may be followed by a field or method
  access: `f()?.x` reads as `(f()?).x`, and so does `v.pop()?.0`.
- `%`, `&`, `|`, `^`, `<<`, and `>>` are integer-only; applying one to
  a float is rejected. Floats admit `+ - * /` and the comparisons.
- Unary `*` dereferences a reference into the place it names, and a
  raw pointer into the place it addresses. Assignment through it
  needs a `&mut` reference or a `*mut` pointer, and a raw
  dereference is an operation the gate covers (`ffi.md`). `**p`
  lexes as the power token, but a binary `**` needs a left operand,
  so in prefix position the pair is two dereferences.
- A turbofish supplies explicit type arguments to a generic call;
  without one, the parameters bind from the argument types.
- Array indexing is builtin with panic-on-out-of-bounds semantics;
  indexing a nominal type resolves through the sealed `Index` and
  `IndexMut` specs (see `types.md`).
- `==` and `!=` are builtin over scalars and addresses; over a
  nominal type they resolve through the sealed `PartialEq` spec, with
  `!=` the negation of `eq` (see `types.md`).
- Calls to `panic(...)` diverge with type `!`.
- `unsafe { ... }` opens the gate for the operations inside and
  evaluates to the block's value; everything inside is checked as
  it was outside. The rule is lexical, so an `unsafe fn` body uses
  a block too, and using an unsafe function in value position is
  refused until unsafe function types land. See `ffi.md`.
- A closure is an anonymous function: `[captures] (params) -> body`,
  where each parameter is `[mut] (name|_) [: type]` and the body is
  one expression, or a block whose value is its last expression.
  A capture is `name` (by value: a move, or a copy for a Copy
  type), `&name` (shared borrow), or `&mut name` (exclusive
  borrow, requiring a `mut` local); captures name locals - `let`
  bindings, enclosing parameters, and `self`; a bare parameter
  list captures nothing.
  `ret` inside a closure returns from the closure.
- `(` opens a closure when the parens hold a `:` at depth zero or
  the matching `)` is followed by `->`; `[` opens one when a
  bracketed capture list is followed by such a group. Both are
  errors anywhere else, so no valid program reads differently. `)`
  and `->` share a line, as with `fn` return types; the body may
  start on the next line, and a `{` body on the next line continues
  the closure since `->` never ends a statement.
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
- `x = v` assigns, and so does `x += v` and each of the other compound
  forms the operator set defines. A declaration always spells `:=`.
- `const x := expr` (or `const x: T := expr`) declares a compile-time
  value: the initializer is evaluated during compilation and the
  binding is immutable. `comp` marks parameters and blocks only, so
  `comp x := ...` is a spelling diagnostic pointing at `const`.
- The left side of an assignment is read as an ordinary expression, and
  whether it names a place is decided after the whole line is read. So
  `g() = 1` is a diagnostic about the place rather than a syntax error,
  which is what lets the diagnostic point at the expression that cannot be
  assigned to.
- A block's value is its last expression; `{}` evaluates to `()`.
- `if cond block (else (if | block))?` and `if pattern := expr block
  (else (if | block))?`; `while` mirrors `if` (both accept
  pattern-declarations).
- `match scrutinee "{" (pattern "=>" expr ("," | ";")?)* "}"`
- `loop block`, `while cond block`, `for pattern "in" expr block`;
  `break`, `continue` (unlabeled); loops evaluate to `()`. The `for`
  rule lives in `control.md`.
- `ret expr?` returns early from the enclosing function.

## Modules and paths

Module declarations come exclusively from `alcy.toml` `[modules]`.
Files not included in `include` and unreachable from declared modules
warn (see `modules.md`).
