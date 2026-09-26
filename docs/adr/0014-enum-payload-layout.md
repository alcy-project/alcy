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

## What has to change

Four sites depend on the payload being a pointer, and the two halves
have to agree on the layout:

- The emitter's enum type is `{i32, ptr}`. It becomes a discriminant
  followed by a payload area sized for the widest variant payload.
- Enum construction allocas the payload tuple, fills it, and stores the
  alloca's address into the slot. It stores each field into the slot's
  payload area at that field's offset instead, and stores no pointer.
- `load_payload_field` loads the payload pointer out of the slot, casts
  it to the payload tuple, and indexes it. It indexes the slot's payload
  area at the field's offset instead, with no load and no cast.
- `payload_tuple` already builds a variant's payload shape, so it is
  the natural starting point for the layout rather than something to
  invent alongside it.

**The offsets need one source of truth.** The lowerer addresses a
payload field with a constant byte offset into the slot's payload area,
and the emitter sizes that area. If the lowerer computes an offset
structurally and the emitter sizes the area through LLVM's own layout
rules, the two can disagree, and the disagreement is a misaligned or
misplaced field that only shows up on some target. So the layout is
computed once, in the analyzer, and published in the checked package for
both to read, the way `FunctionMeta` and the generic instantiation
tables already are. Neither half recomputes it.

An area whose alignment exceeds the discriminant's is expressed as an
array of a carrier primitive chosen to carry that alignment, so the slot
is aligned for the widest payload it has to hold rather than for the
discriminant alone. A bare byte array would be align 1, which stores
correctly on x86 and is still wrong on a strict-alignment target, and
this language is meant to reach bare metal.

## Alternatives considered

- **Keep the pointer and forbid returning an enum with a payload.**
  Rejected: `Option` is the language's absence type and every container
  accessor returns one, so this would rule out the standard library's
  core abstraction.
- **Reference-count the payload.** Rejected: a runtime the language has
  not committed to, on a path that should be zero-overhead.
