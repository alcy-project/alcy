# Compile-Time Evaluation (Bootstrap)

`comp` marks code that MUST be evaluated during compilation. Its first
client is `fmt`: format strings are parsed at compile time and expand
to copy sequences (see the fmt API design). This document covers the
stage where `comp` marks parameters and blocks; a compile-time value
in a declaration or an item is a `const` (see `items.md`). It extends
`grammar.md` (which covers MVP only).

## Syntax

`comp` annotates two positions:

```text
params     := (("comp")? pattern ":" type ("," ...)* ","?)?
primary    := ... | comp_block
comp_block := "comp" block
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

## Comp-known values

A value is comp-known when the compiler can produce it during
compilation: literals, `const` items and bindings, and `comp`
parameters (per call-site specialization). Types admitting comp-known
values are integers, `bool`, `str`, and tuples, structs, and fixed
arrays composed of comp-known values.

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
- Calling one outside comp evaluation, in a call or a value position,
  is a compile-time error. There is no `const fn`; `comp` is the one
  word for code evaluated during compilation.
- A receiver method with the marker is deferred; `comp fn` is a free
  function for now.
- Only a tail `ret` is supported by the AST evaluator; an early `ret`
  reached through a branch is a compile-time error until the IR
  interpreter lands (see Deferred).

## Blocks

- A `comp` block evaluates its body during compilation; the body's
  value MUST be comp-known and becomes the block's spliced value.
- Control flow MUST NOT cross a `comp` block boundary except through
  its value: `ret` inside a `comp` block is a compile-time error.
  `break`/`continue` confined to loops inside the block behave
  normally.
- Calls inside comp evaluation interpret the callee with comp-known
  arguments. Using an unsupported operation there is a compile-time
  error pointing at the operation, not at the call boundary.
- `?` evaluates normally; an `Err` reaching the block boundary splices
  as an ordinary comp-known `Result` value.

## Restrictions

The following are compile-time errors inside comp evaluation:

- I/O (`print`, `println`) and `panic` (`panic` aborts nothing at
  compile time; it diagnoses instead).
- Reading `static` items (storage exists only at runtime).
- Loops whose conditions are not comp-known; comp-known loops are
  unrolled.
- References in the spliced result, except `str` (references may appear
  during evaluation; the compiler materializes them as constant data).
- Exhausting the evaluation bound. Evaluation is bounded; the bound is
  implementation-defined but MUST be deterministic for given inputs.

Moves, copies, and borrows inside comp evaluation follow the ordinary
ownership rules; comp evaluation has no runtime effects.

## Diagnostics

Comp failures diagnose with source spans at the offending operation:
non-comp-known argument, inexpressible operation, boundary crossing,
unbounded evaluation, and unreadable statics each produce a distinct
compile-time error.

## Deferred

- Receiver methods with the `comp` marker.
- `comp` in type positions (array lengths) and `const`-position
  extensions.
- Floating-point evaluation, recursion policy beyond the step bound,
  and evaluation caching.
- Generics interplay; user-facing conditional compilation.
