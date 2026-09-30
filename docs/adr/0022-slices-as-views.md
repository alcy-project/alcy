# ADR-0022: Slices, and the Shape of a Reference to One

- Status: Accepted
- Date: 2026-09-30

## Context

`str` was the only view in the language. A `Vec` or a `String` can hand
out a reference to one element, but nothing can name a run of elements,
so a whole-buffer accessor is inexpressible and every walk is an index
loop with a bounds check per step. ADR-0010 rejected `&[T]` as
premature when typed heap primitives landed; the containers are in
place now, and the borrowed APIs they need are the reason to revisit
it.

The constraints are the ones the reference model already has. A
reference is a thin pointer to a place, and borrow checking reasons
about places and paths through them. `str` is already a `{bytes, len}`
pair carried by value, and the standard library's realloc-conflict
rule depends on a view keeping the loan of the buffer it reads
(ADR-0012, `ownership.md`).

## Decision

**A slice is unsized; only a reference to one exists.** `[T]` parses
as a type, and a `[T]` that is not behind a `&` is rejected by the
checker. `&[T]` and `&mut [T]` are the only slice types a program can
name.

**A slice reference is the pair, by value.** `&[T]` is `{ptr, len}`,
not a pointer to a `{ptr, len}`. A slice has no single element to
designate, so the view is the value; a thin pointer would make a
returned view point into the frame that built it. Layout, the call
ABI, and mangling follow: the type is two words, and its symbol
encodes the element type, with the length riding the value. `str`
keeps its own name and the same shape rather than becoming `&[u8]`.

**A fixed array decays at argument positions.** `&[T; N]` feeds a
`&[T]` parameter and `&mut [T; N]` feeds `&mut [T]`, keeping the
borrow's kind and moving the count into the view. A `&mut` source also
feeds a shared parameter through the existing `&mut`-to-`&` reborrow.
The decay is an argument coercion, not a value conversion: no array is
copied.

**Indexing a slice is a runtime-bounds-checked element place.** The
check compares against the length in the view and panics when out of
range, as for a fixed array. Lowering emits `ElemOffset`, whose
destination carries the element reference: the borrow checker extends
the buffer's place with one element step, so a loan into an element
conflicts with a write to the whole buffer and vice versa, while two
element loans that do not alias stay independent.

**Reading a field out of the fat pointer only anchors the buffer
half.** The pointer half names the buffer the view reads through, and
a copy of it carries the view's loan; the length half is a plain
integer that owns nothing, so `n := slice_len(s)` does not extend
`s`'s loan. Anything else would keep a realloc conflict alive past the
view's last use.

**Construction is a core intrinsic pair.**
`slice_from_parts<T>(ptr: &T, len: usize) -> &[T]` and its exclusive
counterpart are signature-checked, so the borrow kind of the result
matches the kind of the source the view was built from.

## Consequences

- `Vec::as_slice`/`as_mut_slice` and `String::as_bytes` exist, and a
  view held across a `push` that reallocates is a reported conflict
  (pinned by `err-slice-realloc`).
- A borrowed API can now take "some run of `T`" without a length
  argument or an index loop, which is the ground the borrowed standard
  library stands on.
- `str` stays a separate type with the same representation; byte-level
  and element-level views do not mix implicitly.
- Out of scope: sub-slicing (`&a[1..3]`) and slice patterns, which
  need range endpoints and arrive with range types (`deferred.md`);
  `get`-style fallible accessors; and coercions between arrays and
  slices as values, which only the reference form supports.

## Alternatives considered

- **`str` as `&[u8]`.** Rejected: it would fold text semantics into
  the general element machinery and rework every string path for no
  capability the separate type lacks.
- **A pointer to the `{ptr, len}` pair.** Rejected: a returned view
  would point at a dead frame, exactly the payload-lifetime fault
  ADR-0014 records, and allocating the pair would put an allocation in
  every accessor.
- **An unchecked element address in the public surface.** Rejected:
  `elem_ptr` exists for the containers that check first; user-facing
  indexing must not carry the unchecked form.
