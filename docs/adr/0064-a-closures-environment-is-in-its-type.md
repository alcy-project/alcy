# ADR-0064: A Closure's Environment Is in Its Type

- Subject: the language
- Status: Accepted
- Date: 2026-10-13

## Context

A closure value is code plus an environment: the captures it saw when
it was written. The compiler lays the environment out as a tuple in
the defining frame and the value points at it, but nothing in the type
says so - `FuncType` is `{params, ret}`, the environment is the
literal's business - and the Copy rule reads every function value as
`Copy` regardless. The consequence is the restriction the capture
check enforces: a capture whose type owns memory is refused, because
a copied function value would copy a pointer to one environment and
the two copies could not both end what it holds (`capture_is_copy`).

That restriction is not a property of the closure machinery; it is
the missing type. This record decides what carries the environment:
a nominal type per closure literal, a move-only function value, or a
`Copy` value with an environment-drop entry that copes by reference
counting.

## Decision

**A closure literal interns a nominal type whose field is its
environment tuple.** The value is still code plus a pointer, and its
`Copy`-ness is the environment's: a closure that captured only `Copy`
values copies, and one that moved a `String` in does not. A closure
coerces to a bare function type (`(T) -> bool`), and that view borrows
the environment rather than owning it, so the value that owns the
environment has to outlive the view - which is the frame-bound caveat
the adapters already document. A non-`Copy` capture moves into the
environment, and the drop pass ends a closure value through its
nominal environment type, the way it ends any other value.

Heap-owning environments are a later slice of the same decision: the
environment is allocated where the closure value can own and drop it,
which needs a drop entry the value carries or an indirection
(`Box<T>`) that exists. Frame-crossing iterators wait for that slice.

## Consequences

The capture check's `Copy` requirement retires for the owning case:
a closure over a `String` is a value like any other, move-only and
droppable, and passing it where a bare function type is expected is a
borrow of its environment. The compiler gains a type per literal
(cheap: the environment tuple is already interned per literal), the
coercion rule from that nominal to a bare function type, and the
drop glue through the environment. What does not change yet: a
closure that escapes its frame is refused, `Vec<T>::sort_by`'s
predicate still borrows its caller's frame, and `Box<T>` waits for the
heap slice that this decision makes room for.
