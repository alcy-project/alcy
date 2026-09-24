# Compile-Time Evaluation (Bootstrap)

`comp` marks code that MUST be evaluated during compilation. Its first
client is `fmt`: format strings are parsed at compile time and expand
to copy sequences (see the fmt API design). This document covers the
minimal stage: three annotation sites with a bounded evaluation
domain. It extends `grammar.md` (which covers MVP only).

## Syntax

`comp` annotates three positions:

```
params    := (("comp")? pattern ":" type ("," ...)* ","?)?
decl_stmt := ("comp")? ("mut")? pattern (":" type)? ":=" expr
primary   := ... | comp_block
comp_block := "comp" block
```

- `fn repeat(comp n: usize, x: i32)` declares a compile-time parameter.
- `comp count := 3` declares a compile-time variable (`comp mut`
  permits compile-time-only mutation).
- `comp { ... }` is an expression evaluated at compile time; its value
  splices into the surrounding runtime code.
- Brace placement follows the Go-style rule in `grammar.md`: the `{`
  stays on the header line.

## Comp-known values

A value is comp-known when the compiler can produce it during
compilation: literals, `const` items, `comp` parameters (per call-site
specialization), and `comp` variables. Types admitting comp-known
values are integers, `bool`, `str`, and tuples, structs, and fixed
arrays composed of comp-known values.

## Parameters

- An argument for a `comp` parameter MUST be comp-known; passing a
  runtime value is a compile-time error.
- Each distinct set of comp arguments specializes the function
  independently (monomorphization-like, as-if semantics; no code-shape
  contract is promised).
- No `comp fn` marker exists at this stage (see Deferred).

## Blocks and variables

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

- `comp fn` markers as explicitness/optimization annotations.
- `comp` in type positions (array lengths) and `const`-position
  extensions.
- Floating-point evaluation, recursion policy beyond the step bound,
  and evaluation caching.
- Generics interplay; user-facing conditional compilation.
