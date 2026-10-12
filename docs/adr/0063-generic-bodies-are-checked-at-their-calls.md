# ADR-0063: Generic Bodies Are Checked at Their Calls

- Subject: the language
- Status: Accepted
- Date: 2026-10-13

## Context

A generic body is checked once per instantiation, with the type
arguments concrete. The checker has no symbolic type parameter to put
in the IR, and monomorphization is the only form the backend sees, so
there is no second form to check against: a body either specializes
for a known type or it is not checked at all.

That has one visible cost. An unbounded generic body that only some
instantiations support fails inside the library that wrote it:
`Vec<T>`'s equality reported the missing `PartialEq` on `vec.al`'s
line rather than at the caller's `==`, and `Vec::sort` still does.
Bounds answered it for the bounded case - the refusal now names the
caller's type (ADR-0028) - and this record decides what remains for
the unbounded one: definition-site checking, an abstract
interpretation of the body with symbolic parameters, or call-site-only
checking, which is today's rule.

## Decision

**A generic body is checked at its instantiations, and nowhere else.**
Bounds are what make that acceptable: a bounded body's failures are
reported where the type is known, and an unbounded body that a bound
would describe takes one instead of a second checking mode.

## Consequences

Definition-site checking would need a symbolic type parameter in the
IR, an account of what a call through one means, and a second answer
to which methods exist for a type that is not one. None of that is in
scope, and nothing needs it yet: the standard library's unbounded
bodies either support every instantiation (`Vec::len`) or are
scheduled to take a bound (`Vec::sort`, which wants `T: Ord`). A
symbolic type would supersede this record rather than amend it, and
the bounded path stays the way a generic body says what it needs.
