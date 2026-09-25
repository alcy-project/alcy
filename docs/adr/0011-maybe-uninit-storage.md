# ADR-0011: Uninitialized Storage is a Type, Not a Runtime Fact

- Status: Accepted
- Date: 2026-09-26

## Context

ADR-0010 made `alloc<T>` return `&mut T` over uninitialized elements.
That signature has no way to express "written" versus "not written", so
`*p` on a fresh element was an ordinary read of type `T` and type
checked. For a `T` that contains a pointer, that read returned
whatever the allocator left in the bytes, and dereferencing the result
was undefined behavior the compiler accepted.

An allocator returning uninitialized memory is not the defect; the
defect is that the type system could not tell the two states apart.
Nothing else in the language had this problem, because nothing else
produced memory the compiler could not account for.

The fix has to be cheap. A growable container writes elements one at a
time and reads them back, so whatever marks the unwritten state has to
cost nothing at runtime and must not need a dataflow analysis to
propagate.

## Decision

Uninitialized storage has its own type, `MaybeUninit<T>`, and the
allocator hands it out:

- `alloc<T>(count) -> &mut MaybeUninit<T>`.
- `elem_ptr<T>` and `dealloc<T>` pass the wrapper through unchanged, so
  the element type of a buffer is a wrapper from allocation to release.
- `uninit_write<T>(slot: &mut MaybeUninit<T>, value: T)` moves a value
  in, leaving the slot initialized.
- `uninit_assume<T>(slot: &mut MaybeUninit<T>) -> &mut T` releases a
  slot as a mutable reference to its value.

`MaybeUninit<T>` is a compiler-owned one-field struct. It has the
payload's layout, and lowering never materializes it as an LLVM
aggregate: it only ever appears behind an opaque pointer, so a store of
a `T` into a slot and a GEP over the buffer both work against the
payload's representation directly. The wrapper costs no size, no
alignment, and no instruction.

Nothing converts a `MaybeUninit<T>` to a `T` implicitly. Using one
where a `T` is expected is a type mismatch with its own message naming
`uninit_assume`. A wrapper has no nominal declaration, so it has no
fields to read and no struct literal that builds one: the only way in
is `uninit_write`, the only way out is `uninit_assume`. The name is
reserved, so the spelling cannot resolve two ways.

`uninit_assume` is a checked-but-not-proven claim, exactly like Rust's
`MaybeUninit::assume_init`: the compiler cannot know whether the slot
was written, so the call is where the obligation is discharged. It is
the one place uninitialized storage can become a `T`, which makes it the
place a future `unsafe` gate belongs.

## Consequences

- Reading an element before writing it is a compile error, including
  the reference case that made the hole dangerous.
- Writing and reading an element are a store and a load. A container's
  hot path is unchanged.
- A container keeps the wrapper in its buffer field and pays one
  `uninit_assume` per read, which is a relabel of the same address.
- The compiler does not track initialization. A slot written on one path
  and read on another is not diagnosed, so `uninit_assume` remains a
  claim the programmer makes. Dataflow tracking is a much larger change
  and buys correctness only for straight-line code, since the buffer is
  behind a pointer the analyzer does not follow.
- `MaybeUninit<T>` is a distinct type, so it cannot be confused with
  `T`, and a function taking `&mut MaybeUninit<i32>` cannot be handed
  a `&mut i32`. Passing uninitialized storage around is therefore
  visible in signatures.

## Alternatives considered

- **Track initialization in the analyzer.** Rejected as a dataflow
  problem: a buffer is reached through a pointer, so "definitely
  written" is not a local fact, and the analysis would have to
  understand aliasing before it could answer. The wrapper gets the
  common case without it.
- **A boolean flag on the reference type.** Rejected. It would not
  survive a signature: a function declared `fn f(p: &mut i32)` would
  accept an uninitialized reference, because the flag lives in the type
  node and the declaration cannot carry it. A distinct type makes the
  obligation survive every hop.
- **Leave `alloc<T> -> &mut T` and document the hazard.** Rejected. The
  hazard is not a documentation problem when the compiler accepts the
  read.
