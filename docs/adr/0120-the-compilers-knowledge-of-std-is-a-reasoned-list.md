# ADR-0120: The compiler's knowledge of `std` is a reasoned list

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-30

## Context

The compiler special-cases standard-library items in several places.
Some are language semantics (layout, operators and the overflow
policy, casts, spec dispatch). Some are performance decisions (a
builtin comparison compiles directly). Some are leftovers of a time
when the library could not express the thing (the `fmt` expansion's
conversions). And two behaviors are currently implemented twice - once
in alcy and once as a compiler path - which is a drift risk that only
grows as the library does.

Without a rule, each new convenience argues for a compiler case, the
library stays where the compiler put it, and the boundary between
"language" and "standard library" erodes. This record fixes the rule
and audits the present.

## Decision

**The compiler owns language semantics; the standard library owns
library policy and algorithms. A compiler shortcut over a library
item must cite one of three reasons, and the list of shortcuts is
kept as a list.**

- The three reasons: (i) it is language semantics; (ii) it is an
  unavoidable cost (the direct builtin comparison, the type-level
  intrinsics); (iii) the manifest or the expansion machinery can
  declare it (ADR-0119's mechanism). A shortcut that can cite none
  is a bug.
- When one behavior has both an implementation and a shortcut, the
  two must agree, and a test names the pair. The implementable side
  is the one that defines the behavior; the shortcut only compiles
  it.
- The audit, as of this record, and each item's disposition:
  - `builtin_display` (the checker's `{}` and `Display`-bound rule
    that admits builtin convertibles with no implementation):
    retires. The builtins gain their conversion implementations in
    ADR-0119's pilot, so the ordinary path reaches them.
  - `is_builtin_comparable` (the direct comparison for builtins,
    used by the operator checks): stays. Tuples and arrays compare
    element-wise structurally, and the direct path is the built-in
    comparison; the alcy implementations exist for bounds and
    generic contexts, and the agreement test pins the two.
  - `panic`'s declaration-free callability: stays; it is the
    language's.
  - The intrinsic closed set: stays; each name is a language or
    platform primitive, and a new name must say which.
  - The recognitions `is_core_fmt`, `is_io_print`, and
    `is_expanded_variadic` (package-gated through the staged index,
    never by bare name): move under ADR-0119's mechanism, or the
    manifest declares which items expand where the machinery cannot
    see it.
  - The `io` write hooks' name mapping (`alcy_print` and friends):
    an ABI detail; it stays, and may move to the manifest later.
  - `fmt`'s conversions: ADR-0119's pilot.
  - `Map`'s `str`-only keys: ADR-0024's continuation; the `Hash`
    capability and the scalar implementations land together
    (ADR-0121).
- The list is a snapshot, not a license: a new shortcut cites its
  reason when it appears. There is no lint for this; the question is
  asked in review, and the answer is in this list.

## Consequences

- The shrinkage is scheduled through the records that own the
  machinery (ADR-0119, ADR-0121), not a flag day, and each removal
  lands with the tests that prove the ordinary path is equivalent.
- The rule cuts both ways: a library that cannot be written in alcy
  must say why, and the answer is usually a primitive the language
  should own - which is where `sys` (ADR-0117) and the `num`
  direction (ADR-0121) came from.
- Reviewers get a concrete instrument: "which of the three reasons?"
  is answerable, and this list is where the answers accumulate.

## Alternatives considered

- **An architecture document without a rule.** Rejected: a list
  without a criterion grows by accretion, and a criterion without a
  list is unenforceable.
- **Automating the audit with a grep-based lint.** Rejected for now:
  the interesting cases are structural (a checker branch, a lowered
  expansion), not string matches, and a noisy lint is worse than a
  review question.
