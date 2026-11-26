# Compile-Time Evaluation (Bootstrap)

`comp` marks code that MUST be evaluated during compilation. Its first client is
`fmt`: format strings are parsed at compile time and expand to copy sequences
(see the fmt API design). This document covers the stage where `comp` marks
parameters and blocks; a compile-time value in a declaration or an item is a
`const` (see `items.md`). It extends the grammar stated in `grammar.md`.

## Syntax

`comp` annotates two positions:

```text
params     := (("comp")? pattern ":" type ("," ...)* ","?)?
primary    := ... | comp_block | comp_if | comp_for
comp_block := "comp" block
comp_if    := "comp" if_expr
comp_for   := "comp" for_expr
```

- `fn repeat(comp n: usize, x: i32)` declares a compile-time parameter.
- `comp { ... }` is an expression evaluated at compile time; its value
  splices into the surrounding runtime code.
- `comp fn name(...) -> T { ... }` is a free function whose call is
  evaluated during compilation (see "Functions" below).
- Brace placement follows the Go-style rule in `grammar.md`: the `{`
  stays on the header line.

A local compile-time value is a `const` declaration:

```text
decl_stmt := "const" pattern [ ":" type ] ":=" expr
```

- The initializer is evaluated during compilation and the binding is
  immutable; `mut` in declaration position folds into the pattern but
  does not survive the const rule.
- `comp` marks parameters and blocks only. A `comp` in declaration
  position is a spelling diagnostic pointing at `const`, because
  `comp` says when code runs and `const` says what a value is.
- `comp if` and `comp for` name a branch and an unrolling the compiler
  performs. Both require a comp context: a comp block, a comp fn, or
  a compiler-expanded body. A plain `if` inside a comp block or comp
  fn already evaluates; the marker is accepted there but adds no
  further effect. Outside a comp context the marker is a diagnostic,
  so a runtime branch must spell itself as a plain `if`.

## Comp-known values

A value is comp-known when the compiler can produce it during
compilation: literals, `const` items and bindings, and `comp`
parameters (per call-site specialization). Types admitting comp-known
values are integers, `bool`, `str`, and tuples, structs, and fixed
arrays composed of comp-known values.

A variadic parameter is read during compilation with `args.len()` and
`args[i]`, where `i` is comp-known. The length is an integer. For an
untyped `args: ..` the indexed element keeps its own type; for `args: T..`
it has type `T`. Values are not exposed there; they lower at runtime as
they did before. An index outside the shape is a compile-time error, and
no runtime type information reaches user code.

## Parameters

- An argument for a `comp` parameter MUST be comp-known; passing a
  runtime value is a compile-time error.
- Each distinct set of comp arguments specializes the function
  independently (monomorphization-like, as-if semantics; no code-shape
  contract is promised).

## Functions

- `comp fn` marks a free function whose calls happen during
  compilation: called from a `comp` block or another comp-evaluated
  context, with comp-known arguments, its result splices as a
  comp-known value.
- A `comp fn`'s body is comp evaluation itself: the restrictions the
  section below lists hold inside it, and it may call other
  `comp fn`s.
- Calling one outside comp evaluation, in a call or a value position,
  is a compile-time error. There is no `const fn`; `comp` is the one
  word for code evaluated during compilation.
- A receiver method with the marker is deferred; `comp fn` is a free
  function for now, and the marker does not combine with `unsafe` yet.
- A `ret` ends the evaluation with its value, whether it is the
  body's tail or reached through a branch: a taken arm hands the return
  to the body that owns it.

## Blocks

- A `comp` block evaluates its body during compilation; the body's
  value MUST be comp-known and becomes the block's spliced value.
- Control flow MUST NOT cross a `comp` block boundary except through
  its value: `ret` inside a `comp` block is a compile-time error.
  `break`/`continue` confined to loops inside the block behave
  normally.
- A call reached from a comp context evaluates an ordinary function
  when its arguments are comp-known; a call in a runtime body is not
  evaluated merely because its arguments happen to be literals.
  Using an unsupported operation is a compile-time error pointing at
  the operation, not at the call boundary. An intrinsic without a
  compile-time form, an external declaration, and I/O or `panic` are
  refused: every effect in the language goes through one of those, so
  comp evaluation cannot reach the world without an explicit new
  form.
- `?` evaluates normally; an `Err` reaching the block boundary splices
  as an ordinary comp-known `Result` value.

## Restrictions

The following are compile-time errors inside comp evaluation:

- I/O (`print`, `println`) and `panic` (`panic` aborts nothing at
  compile time; it diagnoses instead).
- Reading `static` items (storage exists only at runtime).
- Loops whose conditions are not comp-known; comp-known `while` loops
  are unrolled. A `for` head unrolls when its `into_iter`/`next` chain
  is comp-evaluable: a range evaluates through the standard library's
  value cursor, with a mutable receiver written back after `next`.
  An array or slice head is deferred because its cursor holds a raw
  pointer and a comp value has no pointer form. A bound that is not
  comp-known is an error at the head, not inside the cursor library.
- References in the spliced result, except `str` (references may appear
  during evaluation; the compiler materializes them as constant data).
- Exhausting the evaluation quota. Every function call, every loop back
  edge and every materialized `for` element spends one unit of a fixed
  quota (one million at this stage), so evaluation terminates
  deterministically; the quota cannot be raised yet.
- Exceeding the expansion cap. One unrolled `for` body may run at most
  4,096 iterations; this is separate from the global quota and keeps a
  wide expansion from crowding out the rest of compilation. `while`
  and `loop` stay on the quota, which is their unbounded-evaluation
  diagnostic.
- An integer overflow in `+ - *`. The checks are always on
  during evaluation, so a constant that would wrap at runtime is
  refused; `wrapping_add`, `wrapping_sub`, and `wrapping_mul` name the
  wrapping forms for code that wants the wrap.

Moves, copies, and borrows inside comp evaluation follow the ordinary
ownership rules; comp evaluation has no runtime effects.

## Diagnostics

Comp failures diagnose with source spans at the offending operation:
a non-comp-known bound, an inexpressible operation, boundary crossing,
unbounded evaluation, expansion-cap exhaustion, a checked arithmetic
overflow, and unreadable statics each produce a distinct compile-time
error. A failure inside a library body pulled in for evaluation is
reported once at the user's head; the library span is secondary.

## Deferred

- Receiver methods with the `comp` marker.
- `comp` in type positions (array lengths) and `const`-position
  extensions.
- Floating-point evaluation, recursion policy beyond the step bound,
  and evaluation caching.
- A comp-time pointer or index form, which array and slice `for` heads
  need; tree-sitter and LSP support for the explicit markers; an
  effect or purity system (fail-closed refusal is the policy today).
- Item selection is a separate axis: `#[when]` in `items.md` gates
  items when a file is read, before comp evaluation begins. A comp
  body runs inside a selected item and never re-evaluates a gate.
