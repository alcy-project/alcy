# ADR-0097: There are no macros

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

Rust's macro system is two systems: `macro_rules!` by example, which
matches tokens and substitutes them, and procedural macros, which run
arbitrary code in the compiler. The regrets are shared with every
language that has them: expansions are a second syntax with its own
name-resolution rules, hygiene bugs are found by users rather than by
the definition, an error points into expanded code the author never
wrote, and a build depends on code that runs before the language's
rules apply.

alcy needs a handful of the same conveniences — `fmt`'s format strings
are the motivating one — and already has the machinery that answers
them: `comp` marks parameters and blocks that are evaluated at compile
time, inside the language's checker (`docs/spec/comp.md`).

## Decision

**The language has no macro system. Compile-time work is written in
the language, as `comp`-marked code, and if macros ever land they are
declarative and hygienic.**

- No `macro_rules!`-shaped by-example macros, no token substitution,
  and no procedural macros in any form.
- A convenience that other languages spell as a macro is a `comp`
  function or a `comp` block: it is parsed, resolved, typed, and
  borrowed like any other code, and its arguments are values rather
  than token streams. `fmt`'s format string is the model — the string
  is read at compile time and expands to copy sequences.
- If a macro system is ever designed, this record sets its floor: it
  is declarative (a macro is not code that runs in the compiler),
  hygienic (names resolve where they were written, not where the
  tokens land), and a macro's expansion is checked in the ordinary
  type system rather than beside it. Lifting the floor reopens this
  record.

## Consequences

- Every line the compiler checks is a line the author wrote: no
  expansion hides a diagnostic, and no name resolution rule lives
  only inside a macro.
- A build cannot run user code (ADR-0102), so a dependency is data
  the compiler reads, not a program the machine executes.
- The cost is stated plainly: a syntactic convenience that cannot be
  expressed as a `comp` call stays absent, and code that would be one
  macro is spelled out or is a generic function.

## Alternatives considered

- **By-example macros, as Rust has.** Rejected: two syntaxes, hygiene
  as a property of the matcher rather than of names, and diagnostics
  that point into expansions.
- **Procedural macros.** Rejected: arbitrary code at build time — the
  build's result would depend on a program the toolchain runs, which
  is the wall ADR-0102 keeps up.
- **Shipping a few built-in macros only (`format!`-shaped
  conveniences).** Rejected: `comp` covers the same ground with
  checked arguments, and a built-in macro list invites the question of
  why a user cannot add one.
