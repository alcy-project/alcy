# ADR-0010: Typed Heap Primitives over Byte Pointers

- Status: Accepted
- Date: 2026-09-26

## Context

A growable container needs to address element `i` of a heap buffer.
The heap primitive available at the time was
`alloc(size, align) -> &mut u8`, so the only route to element `i` was
to offset a `&u8` by `i * sizeof(T)` and read bytes.

That does not compose. No cast between reference types exists in user
code, so a byte address cannot be turned into a `T`: a container
holding a `&u8` buffer has no way to produce or store an element of
type `T`. Every alternative traded away something worth keeping:

- **A byte-level offset plus a cast.** A cast from `&mut u8` to
  `&mut T` is a type assertion the compiler cannot check. It is the
  hole through which every type-unsoundness in a byte-buffer
  container would enter, and it would be reachable from ordinary user
  code.
- **Indexing on `&T`.** `p[i]` on a bare `&T` has no length to check
  against, so it is unchecked by construction, and it puts the escape
  hatch in ordinary syntax.
- **A slice type `&[T]`.** Cleanest, and the largest type-system
  addition of the three.

The decision that follows: a byte-level pointer is the wrong unit.
Element addressing must be typed, and the type must be part of the
allocation primitive's signature so the compiler supplies the element
size and alignment rather than the caller.

## Decision

Element addressing is typed, and the type appears in the allocation
primitive:

- `alloc<T>(count: usize) -> &mut T` reserves `count` elements at
  `T`'s own size and alignment. The compiler multiplies and queries
  the layout; the caller never states a byte count.
- `dealloc<T>(ptr: &mut T, count: usize)` releases it.
- `size_of<T>() -> usize` and `align_of<T>() -> usize` expose the same
  layout the compiler used, so a container can size a growth step
  without duplicating the arithmetic.
- `elem_ptr<T>(ptr: &mut T, index: usize) -> &mut T` is the typed
  element offset. It is unchecked by design: the reference carries no
  length, so the check belongs to the container that owns one. This is
  the same division `str_byte` already follows.
- `*p` dereferences a reference into the place it names, and requires
  a `&mut` reference to be assigned through.

No primitive takes or returns a byte count or a `&u8` for a typed
buffer, so no spelling lets a caller assert a type the compiler did
not allocate.

The intrinsics are generic, and that is what keeps them inside the
closed set. `elem_ptr` and `dealloc` recover `T` from the pointee of
their reference argument, so a call admits exactly one instantiation
and no inference search is needed. `alloc`, `size_of`, and `align_of`
have no such argument, so `T` comes from a turbofish, which is also
unambiguous. Each still declares its canonical shape, checked with the
type parameters bound to a placeholder, so a generic intrinsic cannot
widen what the compiler implements.

`alloc` changing from byte counts to element counts is a breaking
change to the intrinsic's signature. It is taken while the only callers
are core and the test suite, rather than after containers depend on the
old shape.

## Consequences

- A growable container becomes writable in core without a cast, a
  slice type, or an unchecked user-facing index. The bounds check
  lives in the container, where the length already is.
- Element access compiles to a typed GEP and element moves to
  load/store: no packing, unpacking, or byte shuffling on the hot
  path.
- A caller can still over-index a buffer by lying about the length it
  passes to `elem_ptr`. That is the same trust `str_byte` already
  requires, confined to the intrinsic set rather than reachable
  through the indexing syntax.
- `alloc<T>` hands back `&mut T` over uninitialized elements, so a
  read before a write is accepted. ADR-0011 closes that hole with
  `MaybeUninit<T>`.
- The `measure` field on `ir::Instruction` and the `ElemOffset`,
  `TypeSizeOf`, and `TypeAlignOf` opcodes exist for this. Layout
  queries are compile-time constants, so a container's growth path
  folds the multiply away.

## Alternatives considered

- `&[T]` slice types: rejected as premature. The single consumer of
  element addressing is a container that already holds its length, so
  a slice type would be a general facility with one internal user. It
  stays the right answer if a second one appears.
- A byte-level `offset_ptr` plus a `&u8` to `&mut T` cast: rejected.
  It is the type-unsoundness this ADR exists to avoid, and it would
  also make a container's element type a runtime claim rather than a
  compile-time one.
- Indexing on `&T`: rejected. It is unchecked by construction, and it
  puts the escape hatch in ordinary syntax instead of the closed
  intrinsic set.
