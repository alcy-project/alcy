# ADR-0014: An enum payload lives in the enum's own slot

- Status: Accepted
- Date: 2026-09-26

## Context

An enum value is lowered as a slot of a discriminant and a payload
*pointer* (`{i32, ptr}` in the emitted LLVM type). Constructing a
variant allocates the payload in the current function's frame and stores
that alloca's address in the slot.

That is fine while the enum stays in the frame that made it, and wrong
the moment it does not. Returning it hands the caller a pointer into a
frame that the call has already left:

```
enum E { A(i32), B }

fn mk(x: i32) -> E { ret E::A(x) }

fn get(e: E) -> i32 {
  ret match e { E::A(v) => v, E::B => 0 }
}

fn sink(e: E) { _ := get(e) }

fn main() -> i32 {
  e := mk(42i32)
  sink(mk(7i32))
  sink(mk(9i32))
  print(format("{}", (get(e),)).as_str())   // 0, not 42
}
```

Nothing in the source is wrong, and nothing about the program is
exotic. It passes today by accident whenever the dead frame happens to
still hold the payload, which is why the suite does not catch it: a
returned `Option<T>` is read while the bytes behind it are still
intact.

This surfaced as a `Vec<T>` fault. A container's accessors return
`Option<T>`, so the moment a container hands a value back to a caller
and the caller keeps it across another call, the value is read out of a
dead frame. That looked like a borrow-checking problem and is not; see
`docs/adr/0012` for the correction.

## Decision

The payload is stored **inline in the enum's own slot**.

An enum's emitted type becomes a discriminant followed by a payload
area sized and aligned for the largest payload among its variants. A
variant with no payload stores nothing, exactly as it does now. Reading
a payload field reads the slot at that field's offset; writing one
stores there. No pointer to a frame outlives anything, so an enum value
can be returned, stored in a struct, put in a container, and held across
calls.

## Why the alternatives do not work

**A caller-provided return slot (sret).** This fixes the slot, not the
payload. The payload is still a pointer, and it would still point into
the callee's frame. sret would have to be combined with this change
anyway, so it is not an alternative to it.

**Heap-allocating the payload.** The payload would outlive the frame,
but nothing frees it: there is no garbage collector, and a payload
pointer has no owner to run a destructor. Every returned `Option` would
leak. Rejected.

**Copying the payload at each return into some longer-lived storage.**
There is no such storage to name. The caller's frame is the only
candidate, which is sret, and the general case has to work for a
payload nested in a struct or an array as well as for a returned value.

## Consequences

- `Option<T>` can be returned and retained, which is what every
  container accessor does, so this unblocks `Vec<T>`.
- The slot grows to the largest variant payload, so an enum costs more
  than the variant in use. That is the same trade every tagged-union
  language makes, and it buys a payload with a home.
- The payload area needs an offset and an alignment per field, computed
  from the variant's payload types. Layout has to be agreed between the
  emitter, which sizes the slot, and the lowerer, which addresses fields
  within it, so both must derive it from the same source rather than
  each computing its own.
- A payload reached as `&mut T` (as `Option<&mut T>` hands out) is
  addressable in the slot at its own offset and alignment, so an
  accessor can return a reference into the caller's own storage. That is
  what a container needs, and it only works because the payload is
  inline.
- Uninhabited payloads (`()` fields) still store nothing, and a
  unit-variant enum still has no payload area.

## Plan

Three commits, each green. The offset source is the analyzer: the
emitter builds the payload area from numbers the analyzer published, and
the lowerer addresses fields with the offsets the analyzer published, so
the two halves cannot disagree.

**1. A layout function over raw types.** `ir::type_layout` returns the
size and alignment of a type for a target width, covering the primitive
tags, references, tuples, structs, arrays, enums, and `str` (whose
length rides the pointer width). Unit tests assert hand-computed values
per width. This is the only new piece of layout knowledge in the
compiler, and a region solver will want the same function.

**2. Publish each enum's payload layout.** The checker computes, per
enum, the payload area's size and alignment, and per variant per field
the byte offset within the area. It goes in `CheckedPackage` beside the
other lowering tables. Unit tests cover an agreeing-shape enum, a
differing-shape one, and a payload-less one.

**3. Store the payload inline.** The emitter's enum type becomes a
discriminant followed by the payload area, built from the published
numbers, and asserts in a debug build that the type it built has the
published size and alignment. Construction stores each field at its
offset; `load_payload_field` reads at its offset. Neither allocates a
payload or stores a pointer any more. The `dangle` reproducer becomes an
`exe` case, and a differing-shape enum (`Result<T, E>`) gets one too.

Two constraints shaped this. A tuple's element type indices must be
consecutive in the type table, so the slot's two halves are appended
back to back with nothing in between; a byte area needs no nested
construction, so the ordering falls out. And the borrow checker's place
is a root plus a field path that already takes every constant index, so
a two-step projection needs no change there and is in fact more precise
than one pointer to the whole payload.

## Alternatives considered
## Alternatives considered

- **Keep the pointer and forbid returning an enum with a payload.**
  Rejected: `Option` is the language's absence type and every container
  accessor returns one, so this would rule out the standard library's
  core abstraction.
- **Reference-count the payload.** Rejected: a runtime the language has
  not committed to, on a path that should be zero-overhead.
