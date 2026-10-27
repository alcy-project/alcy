# ADR-0092: A conversion is a closed list, and a method is found by name

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

Rust's regret here is `Deref`: one user-defined trait drives both
coercion and method resolution, so what a call resolves to depends on
a chain of implementations the call site does not name. `&String`
reaches `str`'s methods and `&Vec<T>` reaches `[T]`'s, which reads
well until an implementation added in a dependency changes which
method an existing call picks. A signature's meaning stops being local
to the signature.

alcy has the shape of the opposite answer already: `unify` accepts
exactly the conversions the language defines, and dispatch tries an
inherent method before the in-scope spec implementations and never
walks through a reference to find one. Nothing records that these
lists are closed, so every convenience conversion is an open question
and the answer drifts with the caller's mood.

## Decision

**The conversions are the ones listed, each a language rule rather
than a user implementation; a method call borrows its receiver at most
one step and never dereferences to find a method.**

- The conversions are: `!` to any type; `&mut T` to `&T`, and
  `*mut T` to `*T` — the same referent without the write permission;
  `&[T; N]` to `&[T]`, and `&mut [T; N]` to `&mut [T]` — the view
  loses its length; and a closure value to the bare function type its
  environment fits (ADR-0064). Identity is not a conversion.
- No numeric conversion is in the list. An `i32` does not reach an
  `i64` parameter, and a value-changing conversion is spelled `as`
  (ADR-0077) or a method that says what it does. A bare integer
  literal takes its type from what the context expects, which is
  typing rather than conversion.
- No `Deref`-shaped spec exists or may be added. A user-declared
  conversion would be a second dispatch mechanism beside the spec
  method dispatch, and a call's meaning would stop being local.
- A method call resolves in one order: the receiver's type, followed
  back to its declaration when the receiver is a storage copy; the
  inherent `impl` block; then the in-scope spec implementations whose
  target matches. Coherence leaves at most one implementation per spec
  and type, so two specs offering one name is an ambiguity error
  rather than a choice.
- The receiver is coerced to the declared receiver by the call: an
  owned place is borrowed (`T` to `&T` or `&mut T`), and a `&mut T`
  is reborrowed shared. Nothing else: a `&T` does not reach `T`'s
  by-value methods, and a `&Vec<T>` does not reach `[T]`'s methods
  without naming the view (`as_slice`).
- The dispatch order is what a reader expects from the type's own
  block: an inherent method shadows a spec's, and a call that spells
  type arguments reaches only a method that declares them (the eager
  inherent methods declare none).

## Consequences

- Adding an implementation cannot change what an existing call means:
  a name resolves by the type's own block and the specs in scope, and
  a spec arrives by `use` (or a prelude), so the change is visible in
  the file rather than in a transitive implementation.
- A conversion that a future feature wants has to argue its way into
  this list, and the argument is about the language's typing, not
  about one library's ergonomics.
- The cost is real: `&Vec<T>` users spell `as_slice`, and the
  standard library keeps the two views (a container and its slice)
  as separate surfaces rather than bridging them silently.
- There is no user-defined `Deref`, so a smart pointer is an ordinary
  type that names its accessor: `Box` hands out `get` and `get_mut`
  rather than borrowing a syntax into existence.
