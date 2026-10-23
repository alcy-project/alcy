# ADR-0083: A value is sized, and an unsized type lives behind a borrow

- Subject: the language
- Status: Accepted
- Date: 2026-10-23

## Context

`docs/spec/types.md` says a slice `[T]` is unsized and appears only
behind a reference, and `str` is a compiler-known view backing string
literals; the intrinsics pass `str` whole (`str_len(s: str)`,
`panic(msg: str)`), so it is a two-word value rather than a pointee.
`Vec<T>` and `String` own runtime-length data through sized fields.
Raw pointers are thin, so an unsized pointee has no representation
behind one.

Generics are being re-cut. ADR-0079 has a generic item cross as its
checked body and a specialization pass substitute concrete type
arguments; ADR-0080 resolves a call on a parameter in that pass. Both
assume every substituted type has a layout, and nothing states that
assumption as a rule. Dynamic dispatch does not exist: ADR-0080
rejected a dictionary beside the value for generic calls, and ADR-0075
lists fat pointers for dynamic dispatch as a decision of its own.

Rust is the counter-example this record exists to avoid. `Sized`
became an implicit bound on every parameter, the unsized types
(`str`, `[T]`, `dyn Trait`) arrived after layout and generics were
fixed, and `?Sized` is a negative opt-out every generic author has to
learn. alcy is pre-MVP with no compatibility to keep, so the axiom is
chosen now rather than patched later.

## Decision

**Every value's size is known at compile time, and a type without a
size has no values: it appears only as a borrow's pointee.**

- A parameter, return, local, field, array element, and type argument
  is a sized type. Layout, moves, aggregate copies, and ABI lowering
  are therefore total over everything a function can name.
- The unsized types are `[T]`, and a trait-object type if dynamic
  dispatch ever lands. Each appears only as the pointee of `&`/`&mut`:
  a borrow of an unsized pointee is a fat, sized value (an address plus
  a length or a vtable), a bare unsized type in a value position is
  rejected the way `[T]` is today, and a thin raw pointer cannot name
  one.
- `str` is a sized compiler-known view: a pointer and a length travel
  together as the value, and it keeps its current shape. It is not the
  model for unsizedness and not a special case of `[T]`.
- **Sizedness is an invariant, not a bound.** No declaration spells
  it, nothing relaxes it, and there is no negative form. The compiler
  derives a type's size from its shape the way it derives `Copy`; a
  declaration never proves it.
- **A type argument is sized.** A slice or a trait object is never a
  type argument; generic code that wants one spells a borrow
  (`&[T]`, `&dyn Spec`). This is what keeps the specialization pass
  (ADR-0079) total: every substitution yields a type with a layout,
  and every instantiation has a size before its body is cloned.
- **No owned unsized value.** `Box<[T]>`, `Box<str>`, and a future
  `Box<dyn>` have no spelling: a value whose size is unknown cannot be
  owned. The types that own runtime-length data are `Vec<T>` and
  `String`, whose buffers are sized allocations behind sized handles.

## Consequences

- Every signature is quiet. No parameter carries a sizedness bound, no
  diagnostic explains a missing one, and a layout query over a type is
  total.
- Generic code over slices and growable sequences is written against a
  borrow. What Rust expresses with `T: ?Sized` is expressed here by
  naming the borrow, and a spec that wants one method for both takes
  `&[T]` or another view.
- Dynamic dispatch, when it lands, is a borrow: `&dyn Spec` is a fat
  value, and the object type is never a value. Owned dispatch has no
  spelling until a decision gives it one.
- The existing language does not change: `str`, `[T]`, `Box<T>`,
  `Vec<T>`, and `String` already fit the rule. The record fixes the
  rule before the generic and layout work lands, so that work builds on
  it rather than around it.

## Alternatives considered

- **Rust's implicit `Sized` bound with a `?Sized` opt-out.** Rejected:
  the negative form is the regret. It makes unsized support leak into
  every generic signature and every diagnostic, and it turns the
  default into something every author must reason about.
- **No rule: let an unsized type appear wherever the operations
  fit.** Rejected: layout, moves, and ABI lowering become partial, and
  a substitution in the specialization pass could produce a type with
  no layout, with the failure surfacing far from what caused it.
- **Sized fat values for every view, removing unsized types.** Give
  `[T]` and a future `dyn` the shape `str` has, so that every
  reference is a thin pointer to a fat value. Rejected: the fat value
  needs a place to live, so a borrow would point at a temporary in
  every call, and the pointer a slice already is would split into two
  representations. `str` keeps its shape because it has no place to
  live: it is the compiler's own view.
- **Owned unsized values through a special `Box`.** Rejected: it
  needs a second representation for every operation that moves or ends
  a value, and the language already has an owning answer for
  runtime-length data (`Vec`, `String`).
