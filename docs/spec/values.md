# Values, Affinity, and Destruction (MVP)

## Value model

- Every value has a single owner. Assignment, argument passing, and
  `return` move ownership. Accessing a moved-from variable is a
  compile-time error (use-after-move MUST be rejected).
- Values are affine (use at most once): unused values are implicitly
  dropped at scope exit. There is no must-consume checking.
- `Copy` is structural and opt-out-free: a type is `Copy` if and only
  if all of its fields are `Copy`, except that a type with a
  user-defined destructor is never `Copy`. Primitive machine types and
  shared references (`&T`) are `Copy`; exclusive references (`&mut T`)
  are move-only. No syntax exists to declare or suppress `Copy`.

## Destruction

- Structs have no constructors. A user-defined destructor (`drop`) MAY
  be defined per type.
- Drop glue is statically placed at every scope exit, including early
  `return` and diverging branches. There is no runtime drop state
  (no conditional-drop flags).
- Drop order MUST follow reverse declaration order within a scope,
  applied recursively to fields in declaration order.
- A drop executes exactly once per value. The panic path aborts
  without running drops (see `errors.md`).

## Shadowing

- Shadowing is permitted. Each shadowing declaration introduces a
  fresh binding; the compiler desugars shadowing before name
  resolution, so later analyses only ever see distinct bindings.
- A `:=` declaration MUST bind only names that are new in its scope;
  redeclaring is a compile-time error suggesting `=`.
