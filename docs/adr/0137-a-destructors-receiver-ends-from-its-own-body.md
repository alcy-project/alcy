# ADR-0137: A destructor's receiver ends from its own body

- Subject: the language
- Status: Accepted
- Date: 2026-11-01

## Context

ADR-0062 has the drop pass place ends: a type that declares a
destructor runs that method, and every other type's fields are walked
by the compiler. ADR-0086 then stated that the receiver is no
exception, that its fields end when the destructor's scope exits, and
`docs/spec/values.md` repeated it. The pass implemented the other
reading: a destructor's receiver was exempt, and the fields it held
ended only if its body ended them.

The exemption was written off as a leak. A type whose destructor does
not end what its receiver holds does leak it, and the tree had no way
to notice, so the walk was given the receiver: its glue call was
suppressed and its fields ended element by element after the body ran.
Every consuming iterator in the std then freed its storage twice —
`ArrayIntoIter<T, N>` holds a `[T; N]` and ends the elements a loop
never took with `drop_in_place`, and the walk's own end of that array
ended the same elements again — and seven exe cases died in `free`.

## Decision

The walk does not end a destructor's receiver. The destructor's body
ends what the receiver still holds, and the compiler places no end of
its own on the receiver or on its fields.

A value that has been partly ended has no spelling. The language has
no `forget` and no `ManuallyDrop`-shaped wrapper (ADR-0013), so every
binding of `[T; N]` ends all `N` elements: a cursor that hands its
elements out by value must keep the storage it has consumed, send the
taken slots away, and end the tail it did not reach. There is no
expression that holds the array and ends part of it, so the field
cannot be handed to the walk at all. Ending it anyway ends the tail
twice; refusing to end it is the only reading of the value that the
language can state.

The std already relies on this. `Vec`, `Map`, `Set`, `String` and
`Box` end what they own from their destructor through
`drop_in_place`, and their fields are views, so no end is owed; a
sequence cursor is the first type that holds a real array, and the
array cursor's landing (ADR-0013) recorded it as the shape a by-value
cursor must have.

## Consequences

- A destructor owns its receiver. What its body does not end leaks,
  and `docs/spec/values.md` states that duty where it states the
  language's other ends. `drop_in_place(&mut self.field)` ends one
  field; moving a field into a binding hands it to the walk.
- The order ADR-0086 states still holds for every end the compiler
  places: a block's values in reverse declaration order, a struct's
  fields in declaration order, an array's elements in index order. The
  order of what a body ends is that body's own, stated by its
  statements.
- `ArrayIntoIter::drop` ends the elements a loop never took, in index
  order, and nothing ends them a second time.
- The exemption matches the receiver's root place alone. A field
  reached through the receiver is not walked, but a value the receiver
  moves out into a binding is an ordinary local and ends normally.

## Alternatives considered

- **End the receiver's fields after the body**, which ADR-0086 read as
  the rule: rejected above. It requires a way to forget, and a
  language that grows one loses the only proof the drop pass has.
- **Require a destructor's body to end every destructible field and
  diagnose the ones it does not**: the compiler cannot tell "ended"
  from "moved into a binding that is still alive" without a dataflow
  over the body it does not have, and a type may end a field on one
  path and not another. The duty is stated instead.
- **A compiler-known array cursor**, whose end the compiler emits as a
  loop from the cursor's position: it would make one std type work and
  leave every user type that owns an array and hands elements out
  unspellable, which is what a by-value cursor is for.
