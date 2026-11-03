# ADR-0136: A size is an offset a pointer can carry

- Subject: the language
- Status: Accepted
- Date: 2026-11-01

## Context

`docs/spec/types.md` promises that every value's size is known at
compile time and that layout, moves, and ABI lowering are total over
everything a function can name. Nothing bounded the number, though: a
fixed array's size is its element's size times its length and an
aggregate's is the sum of its fields, and both were computed in `u64`
with no check. `[[[[u8; 1048576]; 1048576]; 1048576]; 1048576]` is
2^80 bytes, which wraps to 0, so `size_of` answered 0 for a type whose
values cannot exist, and a value of it would have been allocated at a
size the target cannot address.

Offsets are the reason the bound is `isize` rather than the wider
`usize`: an element is reached by adding an `isize`-wide offset to the
base of its array, and address arithmetic that leaves the `isize`
range is undefined. A type above `isize::MAX` therefore has no layout
the program could compute rather than one the compiler failed to.

## Decision

**A type whose size does not fit `isize` has no layout, and forming it
is an error.**

- The size of a shape is the size the layout reports: an array is its
  element times its length, an aggregate is its fields aligned and
  summed in order, and a variant earns only its widest payload.
- The analyzer answers a shape's size before it interns the type, and
  refuses a shape that does not fit with `EC050`, so no type that
  reaches lowering has a size the target cannot address.
- A shape that still holds a type parameter or a const argument has
  no size yet; the instantiation that binds the argument forms a
  shape of its own and is asked then (ADR-0079).

## Consequences

- `size_of::<[[[[u8; 1048576]; 1048576]; 1048576]; 1048576]>()` is
  refused instead of answering 0, and an aggregate whose fields
  each fit but whose sum does not is refused the same way
  (`err-type-too-large`, `err-type-too-large-fields`).
- The bound is not a practical limit: 2^63 bytes is beyond any target
  the compiler emits for, so the rule only names what wrap-around
  would otherwise answer.
- A shape that holds itself has no size to bound, and the walk says so
  rather than asking the layout for the number: the layout follows the
  fields it is given, so a cycle left the walk where it started and the
  compiler without a stack. The cycle checks refuse the shape (`EC007`)
  and are the only voice on it (`err-recursive-type-without-indirection`).
- `usize` arithmetic inside a program is unaffected; the rule bounds
  the compiler's layout, and a program that computes its own offsets
  keeps the panics `docs/spec/values.md` gives them.
