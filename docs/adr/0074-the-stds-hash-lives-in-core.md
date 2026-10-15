# ADR-0074: The std hashes with XXH3-64 in `core`

- Subject: the language
- Status: Accepted
- Date: 2026-10-16

## Context

`Map` hashes its string keys with an FNV-1a that lives inside
`lib/std/collections/map.al`, and `Set` answers through `Map`. ADR-0024
recorded that choice as provisional — "when `spec` lands, a `Hash`/`Eq`
generic key is the natural [successor]" — and `spec` has landed since.
ADR-0073 removes FNV from the project's own code, so the provisional
hash needs a real home and a real algorithm now, not at the spec's
leisure.

The std's layout bounds the answer. `core` is "language primitives,
with no dependency on any other package" and holds the vocabulary
modules (`cmp`, `ops`, `index`); `collections` depends on `core` and
`alloc`. The suite's members are an explicit list and a dependency's
closure is required rather than pulled in (ADR-0016), so a new package
pushes its name into every manifest that names members.

## Decision

**The std's one hash is XXH3-64, implemented in alcy in
`lib/std/core/hash.al`, and `Map` hashes through it.**

- The implementation is XXH3-64 over the default secret, scalar, and
  byte-exact with the vendored C library's `XXH3_64bits`; cases hash
  the same inputs on both sides so a drift is a test failure.
- 64 bits, because a map's collision is settled by key equality and
  the slot is a `u64`; the 128-bit width stays where a digest is
  recorded and no comparison follows (ADR-0073).
- The private FNV-1a in `map.al` is removed in the same slice, and the
  `Hash`/`Hasher` specs, when they land, join in `core` too.
- No `hash` package for now: one small module does not justify moving
  the suite's closure into every manifest that names `core`, and the
  primitives' home is where this one belongs. If the surface ever
  becomes something callers select, promoting the module to a package
  is a mechanical move.

## Consequences

Map iteration order is unspecified either way and stays that way; what
changes is the algorithm behind the slots. The project's hash family is
one: XXH3 in the compiler (ADR-0073) and, one width down, in the
standard library the compiler builds — which is what lets a case pin
the language's implementation against the C library's.
