# ADR-0094: A variant set and a method set are promises

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

Two rules make the checks useful: a `match` is exhaustive over an
enum's variants, and every method a spec declares must be implemented
with a matching signature. Both make growth a breaking change. Adding
a variant breaks every exhaustive match; adding a method breaks every
implementation, even one that would not use it.

Rust met this with `#[non_exhaustive]`, an attribute that tells
dependents their match must carry a wildcard arm. The wildcard is a
promise to ignore future variants — including the ones a caller would
want to handle — and std ended up shipping enums whose variant sets
are frozen anyway. alcy has no attribute system at all
(`docs/spec/deferred.md`), so the attribute is not available, and the
sealing mechanism that already exists answers half of the problem:
a spec the manifest lists in `[spec] suite-only` can only be
implemented inside its suite.

## Decision

**An enum's variants and a spec's methods are its declaration. Growth
happens by declaring a new type or a new spec, never by extending one
that others already match or implement.**

- Adding a variant to an existing enum is a breaking change. The
  standard library therefore ships an enum only where the set is
  complete for the question it answers (`Option`, `Result`,
  `Ordering`, `Bound`), and a set that is open-ended — a platform
  error, a protocol status — is a struct whose cases are values and
  whose questions are methods.
- Adding a method to a spec is a breaking change for every
  implementation. A capability grows by a *new* spec that names the
  old one as its super-spec: `spec Eq: PartialEq` is the shape, and
  an implementation of the new spec must carry the old one for the
  same target.
- A spec whose method set must be able to grow is sealed to its suite
  (`[spec] suite-only`). Its implementations are the suite's own, so
  the suite may add methods to it and update them in one commit;
  nothing outside can be broken. A spec that stays open is declaring
  that its method set will not grow.
- There is no `#[non_exhaustive]`, no attributes, and no wildcard
  requirement. A match that covers an enum's variants is exhaustive
  and stays exhaustive; an implementation that carries a spec's
  methods keeps compiling.
- Before 1.0 this record is a design discipline and not yet a
  compatibility promise: a variant or method that turns out wrong is
  removed and every user updated, which is cheaper now than the
  alternative ever is later. The list above is what 1.0 freezes
  (ADR-0099).

## Consequences

- A dependent's `match` never needs a defensive arm, and a warning
  about a new variant is a compile error rather than a silently
  ignored case.
- The standard library pays for this with more types: a growth-shaped
  set is a struct or a fresh enum beside the old one, and a
  capability that grows is a super-spec chain rather than one wide
  spec. Both are readable at the use site, which is the trade
  accepted.
- Sealing is now a design tool rather than a lock: a spec that may
  grow is sealed, and sealing is visible in the manifest where the
  question is decided.
- An ecosystem's error type is its own enum; a new cause is a new
  major version for that library, which is the honest signal.

## Alternatives considered

- **`#[non_exhaustive]` and wildcard arms.** Rejected: it needs an
  attribute system, it forces a wildcard that swallows cases the
  caller would handle, and it converts a compile error into a
  runtime surprise.
- **Open enums by default, matched with a fallthrough.** Rejected:
  exhaustiveness is what lets a match be checked at all, and the
  forgetting it prevents is the common case.
- **Provided methods on specs, so additions are non-breaking.**
  Rejected: a provided method is a second dispatch path with its own
  rules (which implementation is called on a shadowing type), and it
  makes a spec's surface unclear at the implementation site. The
  super-spec chain costs one `impl` line instead.
