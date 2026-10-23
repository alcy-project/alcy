# ADR-0089: A shared borrow reads, and an exclusive borrow is alone

- Subject: the language
- Status: Accepted
- Date: 2026-10-23

## Context

The borrow checker already proves the two rules that make references
usable: a `&mut T` is exclusive for its lifetime, and a `&T` keeps its
pointee alive. `docs/spec/values.md` says a `&T` is `Copy` and a
`&mut T` is move-only, and `deferred.md` defers interior mutability.
Nothing yet states what those two references *promise the optimizer*,
and the LLVM emitter already acts on that promise: it marks an
exclusive borrow's parameter `NoAlias` (`src/codegen_llvm/`'s emitter).
A promise the compiler uses but the language does not state is a
promise that can drift.

Rust's history is the near-miss. `&T` was assumed read-only in
optimization from early on, interior mutability arrived as a special
case (`UnsafeCell`) carved out of that assumption, and the carve-out
is a compiler-recognized hole in the reference rules. Whether alcy has
a hole, and where it is, is cheaper to decide before the optimizer
relies on the assumption than after.

Raw pointers already sit outside every promise: they may be null, they
may alias, and producing or using one is behind the `unsafe` gate
(ADR-0050).

## Decision

**A shared borrow reads its pointee and an exclusive borrow is alone;
interior mutability does not exist, and a future one is a wrapper
that opts out explicitly.**

- `&T` grants reads. Nothing reachable through shared borrows may be
  written while any of them lives, and no operation converts a shared
  borrow into a writable path.
- `&mut T` is unique: while it lives, no other reference reaches the
  same place. The borrow checker proves it; the language states it
  because the optimizer relies on it.
- **Both are optimization promises, not only rules.** A shared
  borrow's pointee is unchanged for the borrow's life, and an
  exclusive borrow's pointee is reached through no other path — which
  is why `NoAlias` on that parameter is sound, and a backend may add
  the analogous `readonly` where a shared borrow is what it sees.
- Raw pointers carry no promise. A reference built from one, inside
  `unsafe`, accepts the reference's promise; the checker cannot verify
  it, and the obligation is the unsafe code's (ADR-0050).
- **Interior mutability is absent.** No type makes the pointee of a
  shared borrow writable. If one lands, it is a compiler-known
  wrapper whose pointee is exempt from the read-only assumption, and
  shared borrows of the wrapper grant only what the wrapper's methods
  offer; checked shapes (a cell that borrow-checks at run time) sit
  above it as ordinary library types. Until it lands, a shared
  borrow's pointee is immutable without exception, and the optimizer
  may rely on it everywhere.
- A field that is a shared borrow keeps the rule; an exclusive-borrow
  field keeps its own (`values.md`: it makes the aggregate move-only).
  Neither is a way around the two promises.
- A foreign function that receives one of these references is bound by
  the same promise, and a declaration is where its caller accepts the
  obligation.

## Consequences

- Function boundaries optimize without whole-program analysis: the
  backend's `NoAlias` parameter attribute is a stated consequence, not
  an implementation guess, and reads through shared borrows may be
  cached for a borrow's life.
- Two exclusive references to one place can never both exist, so no
  runtime token or check exists anywhere in the language.
- The absence is what makes the future wrapper small: one type with
  one exemption, rather than a modifier every borrow must carry or a
  rule the optimizer must weaken globally.
- A program that needs interior mutability today writes `unsafe` and
  keeps the obligation itself; when the wrapper lands, that code has a
  safe spelling and keeps working.

## Alternatives considered

- **Rust's route: assume read-only, carve out `UnsafeCell` later.**
  Rejected: the carve-out becomes a compiler special case retrofitted
  into the reference rules, and every consumer of the assumption has
  to be revisited. Deciding the wrapper's place now, even as a
  reservation, keeps the assumption and its hole in one record.
- **No promise: references may alias, and the backend assumes
  nothing.** Rejected: it gives up the information every function
  boundary depends on, for a freedom nobody asked for — no program
  wants a shared reference that may be written through.
- **Permanent immutability, with no wrapper ever.** Not chosen: a
  cache or a shared counter will eventually need a safe spelling, and
  a permanent ban would push them to `unsafe` or statics instead of
  one wrapper type. The reservation costs nothing until then.
- **Make the promise only about the borrow checker, not the
  optimizer** (references are exclusive as a rule, but no aliasing
  attribute follows). Rejected: the emitter already writes `NoAlias`,
  so the promise exists in fact; leaving it unstated is the drift this
  record exists to prevent.
