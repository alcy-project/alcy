# ADR-0055: Compile-time variadics, and print in io

- Subject: the language
- Status: Accepted
- Date: 2026-10-07

## Context

`fmt::write` and `fmt::format` take the arguments tuple as one
parameter and check its shape customly, because the language cannot
name a varying arity (`fmt.md`). Callers spell `(a,)` and `(a, b)`,
which reads as data where the call site means arguments. `print` and
`println` live in `core` over `sys_write`, so they cannot see a format
string, and `core` cannot depend on `fmt` without inverting the
suite's layering. A hello world selects `core` alone today and would
carry `fmt` and `io` to reach a format string otherwise.

## Decision

**A parameter written `args: ..` takes the call's remaining
arguments.** The variadic parameter is the last one; the compiler
collects the arguments and the expansion sees them as the tuple it
already receives, so no calling convention changes and no C variadic
is involved (`ffi.md`'s variadics stay deferred). At first only the
compiler-expanded functions may declare it - `fmt`'s `write` and
`format` and what `io` builds on them - and a user-declared variadic
is a diagnostic until its own slice.

**`print` and `println` move to `io`.** `io` gains a `fmt`
dependency, which is upward in the suite and not a cycle, and
declares them over a format string:

```alcy
pub fn print(comp fmt: str, args: ..)
```

`println("hi")` is the zero-argument case of the same declaration as
`println("x={}", x)`. They are compiler-expanded like `write` and
`format`, recognized by their package rather than by name, and their
declared bodies never execute (`panic` in the body is the guard, as
in `fmt`). `core` keeps `panic`, and the compiler's name-based
fallback for `print` and `println` retires, because every name in
scope now traces to a manifest entry.

**The minimal hello world names `core`, `fmt`, and `io`.** Selecting
`io` alone does not surface `fmt`'s names, because only selected
members become prelude facades (ADR-0016), so the samples and the
documentation show the three, or the whole suite.

## Consequences

Format call sites read as calls, and the arity check stays where it
is: in the expansion, at compile time. The `fmt.md` API section is
rewritten with the new signatures, and the grammar's parameter
production gains `..` (both grammar files in the same commit; the
tree-sitter grammar follows). Programs that select only
`alcy/std/core` lose `print` and `println`; the suite glob and the
three-member spelling keep working, and `panic` stays reachable
everywhere.

## Deferred

- User-declared variadic functions; the tuple is the expansion's
  business, and a general mechanism needs its own design.
- C variadic calls, which remain in `ffi.md`.
