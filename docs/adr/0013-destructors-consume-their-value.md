# ADR-0013: Destructors consume their value

- Status: accepted
- Date: 2026-09-26

## Context

`alloc` hands back an owning reference and `dealloc` releases it, but
nothing tied the two together. A value that held a buffer simply ended
at scope exit and the block leaked. `items.md` already says so: there is
no garbage collector, and dropping an owned reference is not a runtime
operation, so a leaked block leaks.

A container therefore cannot be written in the language. `Vec<T>` has to
reallocate, and the old block has to be released at the moment the
vector replaces it — code the type itself has to carry, run at a point
the language currently has no way to express.

## Decision

A type declares a destructor as a method named `drop`:

```
struct Res { buf: &mut MaybeUninit<u8> }

impl Res {
  fn drop(self: Res) {
    dealloc(self.buf, 1 as usize)
  }
}
```

The receiver is taken **by value**. Scope exit places the call, passing
the value in, so ending a value is a move like any other.

### Why the receiver is by value

The alternative is `&mut Self`, which is what Rust and Zig both use.
Taking the value has three consequences that make it the better fit for
alcy, and one that does not:

- **It composes with the affine model that already exists.** Ending a
  value is a move, so a value that has already left is not ended again,
  with no separate "was it dropped" flag to thread through. A
  `&mut Self` destructor cannot be called on a value behind a borrow at
  all, and needs a runtime flag to avoid the compiler ending a value the
  destructor already ended.
- **It makes early release work.** `res.drop()` moves, so the value is
  gone and scope exit skips it. That is the common case for a systems
  language: release a buffer before a long operation rather than at the
  end of the function.
- **A destructor may call the type's own methods.** It owns the value, so
  it can walk its elements through `&mut` methods on values it holds.
- **The cost:** a destructor cannot run on something behind a shared or
  exclusive borrow, because moving out of a borrow is not sound. Ending
  an owned value is the case that matters, and `&mut` methods cover
  reading and writing in place.

### Why `Copy` excludes a destructor

`Copy` is structural and opt-out-free: a type is `Copy` if and only if
all of its fields are. A `Copy` type therefore has no owned resource for
a destructor to release, and each copy would be ended. Rather than break
the structural rule by making "has a destructor" an extra condition, a
`Copy` type is simply not allowed to declare one. Rust makes `Copy`
explicit and rejects the combination; alcy has no such syntax, so the
structural rule decides it on its own.

A generic type's `Copy`-ness depends on its instantiation, so a
destructor on `impl<T> Name<T>` is resolved per instantiation: it is
valid where the type is move-only and an error where it is not. A
container's own state decides this, not the element type, so the
container case is unaffected.

### Why the name is reserved

`drop` as a method name with a fixed signature means a destructor cannot
be defined by accident with a shape that would silently never run. A
free function named `drop` is not provided, because ending a value
through a free function is the classic way to end one that is still
owned by a scope the caller cannot see.

## Drop glue

The analyzer computes, for every type, whether ending a value of that
type runs code, and which method to call. A struct whose fields are
destructible gets no glue of its own; the compiler ends each destructible
field directly, so an ordinary wrapper needs no destructor.

Placement:

- A block's trailing value is lowered before the block's exit, because
  evaluating it can move a value out, and ending a value that has already
  left would run its destructor on nothing.
- A `return` runs the destructors of everything still alive, then
  returns. The result is taken first, for the same reason.
- A destructor's own receiver is exempt. Otherwise the destructor's
  scope exit would call the destructor on its own receiver, forever.

Which values have already left is a property of the path, not of the
function. A block snapshots the flags it inherited and restores them on
the way out, so a destructor placed on one path out of a branch does not
retire the value for the code that follows the branch.

Discarding a value that holds something with a destructor is an error.
Nothing would be left to own it, so its destructor could never run.

## Consequences

- `Vec<T>` and a growable `String` can release a block they no longer
  own, and release it automatically when the value ends.
- Destructors do not run on the panic path, which aborts. A buffer lost
  to a panic is leaked, which is the same guarantee a systems language
  gives.
- Reaching a destructor through an enum variant or an array element needs
  the containing type to declare its own `drop`; the compiler warns when
  it cannot place one. Doing it automatically needs a discriminant read
  and a branch, which is follow-up work.
- A partially moved value is not tracked: if one field of a struct is
  moved out, the struct's destructor still runs and ends that field
  again. Field-level move tracking is follow-up work.
- There is no `Drop` bound to name "this type ends with code", so a
  generic function cannot yet be written that requires it. The `spec`
  system covers that (see ADR-0011 and `deferred.md`).

## Alternatives considered

- **`&mut Self` receiver.** Rejected: needs a runtime "already dropped"
  flag to avoid double-dropping, and cannot end a value behind a borrow.
- **A `spec` (trait) bound.** Deferred: `spec` definitions and dispatch
  do not exist yet, and the reserved method name covers the language
  requirement without them.
- **Automatic drop for enum variants and array elements.** Follow-up
  work: both need control flow the drop emitter does not build yet. Until
  then the compiler warns rather than leaking silently.
