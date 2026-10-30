# ADR-0122: A spec may declare an associated function

- Subject: the language
- Status: Accepted
- Date: 2026-10-30

## Context

A spec's members are methods: every one takes a receiver, and the
receiver is how dispatch reaches the implementation (`eq`, `index`,
`next`, `display`). ADR-0121's first target needs the other shape.
`parse` is a capability of the type being parsed, not of the text: the
natural bound is on the result type (`T: Parse`), and the call reads
from the type (`T::parse(text)`, `i64::parse(text)`). A
receiver-taking spec cannot express that. With the receiver on `str`
(`spec Parse<T> { fn parse(self: &Self) -> Option<T>; }`), the
implementations are one per output type on `str` - `impl Parse<i32>
for str`, `impl Parse<u64> for str`, ... - and a bound on a parameter
has no spelling: `T: Parse` names a spec whose receiver is `T`, which
this shape does not provide. The parsers would also sit on `str`
rather than with the types that parse.

A `Default`-shaped capability (a value from nothing, for a clap-like
argument parser's defaults) wants the same shape, so the extension is
not input-specific.

## Decision

**A spec may declare an associated function: a member with no
receiver. `Self` names the implementing type in its parameter and
return types, and an implementation supplies it like any other
member.**

- Declaration: `fn parse(text: str) -> Option<Self>;` inside
  `spec Parse`. Nothing else about the spec system changes:
  implementations are matched by name and signature as before, the
  receiver kind (`none`) is part of the signature, and the rule that
  every declared member must be implemented stands.
- Calls come in two forms, both through the type:
  - A concrete head: `i64::parse(text)` reaches the implementation
    for that type (or the width's spelling of it, as `usize` is
    `u64`'s). The parser admits a scalar spelling as a path head when
    a member follows (`i64::parse`), and only then.
  - A type-parameter head: `T::parse(text)` resolves at the
    instantiation, the way a receiver call on `T` does (ADR-0063):
    the body is checked per instantiation, so a concrete
    implementation is what the call sees.
- No method form lands with it: `str.parse::<T>()` would need a
  generic method with its own parameters on a builtin compound target
  (`impl str`), which the checker does not index; that is its own
  slice, and until then the two forms above are what replaces the six
  names (ADR-0121, amended).
- The `parse` family lands on it: `core` declares `Parse` with
  implementations for `i64`, `u64`, `i32`, `u32`, and `f64` - the set
  the six `str` methods covered - and `usize` is served by the
  width's implementation, the way the comparison implementations
  serve it. `i64::parse(text)`/`T::parse(text)` replace
  `parse_i64`/`parse_u32`/..., and `String`'s six delegations
  collapse into one generic method. The input layer's `next::<T>`
  (ADR-0118) is the same bound.
- `Parse` is open, not suite-sealed: parsing a type of one's own from
  text is the type's own business, and ADR-0090 puts the
  implementation where the type or the spec lives.

## Consequences

- A bound can demand a static capability: `T: Parse` is nameable, and
  a call site reads `T::parse(text)`.
- The spec system's surface grows by one shape, not one mechanism:
  dispatch, implementation matching, sealing, and generic methods
  are unchanged.
- The concrete spellings are replaced, not aliased (pre-1.0, per
  ADR-0121); the exe and e2e cases move to `parse::<T>()`.

## Alternatives considered

- **A receiver-shaped `Parse` on `str` (`spec Parse<T> for str`).**
  Rejected: a bound on a parameter has no spelling, the
  implementations pile onto `str` instead of the parsed types, and
  the scanner's `next::<T>` needs exactly the bound.
- **A free function `parse::<T>(text)` over the same spec.** Not
  taken: the member would still be receiverless, so the extension is
  the same, and the type-first calls cover what a free spelling
  would, without a third name for one capability.
- **Reflection over a type's conversions.** Rejected: heavier than
  the shape needs, and ADR-0119 keeps reflection for the data-shaped
  clients.

## Deferred

- Default methods (ADR-0121's iterator note).
- Associated values (constants) on specs.
