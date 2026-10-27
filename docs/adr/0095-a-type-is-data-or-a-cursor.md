# ADR-0095: A type is data or a cursor, not both

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

`Range` began as both the interval and the walk over it, the way
Rust's `Range` is an `Iterator`. The regret is concrete: `for i in r`
consumes `r`, so a second loop over the same binding silently
iterates nothing, and the interval — which is data a caller passes
around — cannot be `Copy` or reused once it has been walked. The
cursor's state contaminated the value.

alcy split them: `Range` holds the endpoints and the walk is a cursor
built from it (ADR-0034, and ADR-0037 for the stride), and the
container cursors (`VecIntoIter`, `VecIter`, `MapIntoIter`, ...)
follow the same shape.

## Decision

**A type that answers "what is the data" does not carry "where the
walk is". The walk is its own type, built from the data by the
operation that needs it.**

- An operation that advances (`next`-shaped) is a cursor: it owns a
  copy of what it walks, or borrows it, and it advances itself.
- The data type stays a value: it is `Copy` when its fields are, it
  is passed and stored like any other value, and it can start any
  number of walks.
- `for` builds the cursor it needs from the expression it is given,
  so the desugaring is visible in the types: a range gets its
  iterator, a container gets its borrowing or consuming cursor.
- A type that wants both surfaces spells them as two: the container
  has `iter`/`into_iter`/`iter_mut`, not a `next` on itself.

## Consequences

- A walk does not consume its subject unless it is meant to: `for i
  in 0..n` twice is two loops, and `r` remains usable afterwards.
- A cursor carries its position and its subject; a value that must be
  stored compactly stores the data type, not a half-consumed walk.
- The cost is one type per walk shape, which is why the naming rule
  keeps the pairs visible (`Range`/`RangeIter`, `Vec`/`VecIter`).

## Alternatives considered

- **One type that is both, as Rust's `Iterator for Range`.** Rejected:
  the value is consumed by walking it, the second use is silent, and
  `Copy` is lost for data that has no state.
- **The language hiding the cursor entirely (a `for` protocol on the
  data type).** Rejected: the protocol would be a spec every data
  type must implement, which is the same coupling under another name;
  the split keeps the cursor an ordinary type a user can write.
