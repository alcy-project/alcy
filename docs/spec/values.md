# Values, Affinity, and Destruction (MVP)

## Value model

- Every value has a single owner. Assignment, argument passing, and
  `return` move ownership. Accessing a moved-from variable is a
  compile-time error (use-after-move MUST be rejected).
- Values are affine (use at most once): unused values end at scope
  exit with no user code running. There is no must-consume checking.
- `Copy` is structural and opt-out-free: a type is `Copy` if and only
  if all of its fields are `Copy`. Primitive machine types and
  shared references (`&T`) are `Copy`; exclusive references (`&mut T`)
  are move-only. No syntax exists to declare or suppress `Copy`.

## Destruction

- There is no destruction in MVP: no destructors can be defined, no
  drop glue is placed, and scope exit runs no user code. Values
  simply end at scope exit; the panic path aborts the same way
  (see `errors.md`).

## Shadowing

- Shadowing is permitted. Each shadowing declaration introduces a
  fresh binding; the compiler desugars shadowing before name
  resolution, so later analyses only ever see distinct bindings.
- A `:=` declaration MUST bind only names that are new in its scope;
  redeclaring is a compile-time error suggesting `=`.
