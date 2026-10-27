# ADR-0104: An unsuffixed literal has a default type

- Subject: the language
- Status: Accepted
- Date: 2026-10-28

## Context

`x := 42` names no type, and `(0..)` names no element type: the
language has to answer what an unsuffixed literal is when nothing
around it asks for one. Rust answers `i32` (and `f64`), and the answer
is felt in two places: a program's arithmetic width is settled by a
default rather than by the code that reads it, and inference crosses
function boundaries, so a call site can decide a parameter's width
rather than its declaration.

alcy's spec already says what it does — a body infers its locals
intraprocedurally, integer literals default to `i32`, float literals
to `f64` (`docs/spec/items.md`), and a range with no endpoints takes
the same default (`docs/spec/types.md`). What is missing is the record
that this is a decision, and the boundary that keeps the default
local.

## Decision

**An unsuffixed literal takes the type its context asks for; where
nothing asks, an integer literal is `i32` and a float literal is
`f64`. The default is decided where the value is bound and never
travels.**

- An expected type wins: a parameter, a binding's annotation, the
  other operand of a binary operation, a struct field, a range
  endpoint, and an index position all pin the literal. A pinned
  integer literal must fit the type it is pinned to — an out-of-range
  literal is an error, not a wrap.
- A suffix wins over both (`42u64`), and a suffix that names the wrong
  family (`42f32`, `1.5i64`) is a type error rather than a
  conversion.
- With neither, the default applies: `x := 42` is an `i32`, `(0..)`
  is a `Range<i32>`, and `x := 1.5` is an `f64`.
- The default does not re-open across a call. `f(x)` where `x := 42`
  and `f` takes a `u64` is a type error, because there is no implicit
  numeric conversion (ADR-0092); a value's width is readable from its
  binding. Inference is intraprocedural and stays that way.
- A `usize` position — an index, a length, a capacity — pins the
  literal to `usize` like any other expected type, so `a[0]` and
  `Vec::with_capacity(8)` read naturally without a suffix.

## Consequences

- Arithmetic on a bare literal is 32-bit unless something asks for
  more, and the build's overflow checks (ADR-0065) apply to it like
  any other arithmetic: the default is a width, not an unbounded
  number.
- A value that must be 64-bit says so where it is bound; a reader
  never has to look at a call site to learn a parameter's width.
- The cost is a suffix where another language would widen silently:
  `1u64 << 40` is spelled, and a program that mixes widths is told so
  rather than converted.

## Alternatives considered

- **No default: a literal needs an annotation or an expected type.**
  Rejected: `x := 1` is among the most common lines a program has, and
  the rule would turn it into a diagnostic.
- **Default to `usize`.** Rejected: it makes the machine's pointer
  width the arithmetic width, so one source computes differently per
  target — the opposite of what a default is for.
- **Let inference cross a function boundary, as Rust's fallback
  does.** Rejected: a callee's signature would be settled by its
  callers, and a width would stop being readable at the declaration.
- **A 128-bit or arbitrary-precision literal type.** Not taken: it
  buys a suffix-free overflow story at the cost of a second arithmetic
  tower (ADR-0066's definedness rules would grow a second case), and
  the checked width is what the program asked for. It is not
  foreclosed for a later major version.
