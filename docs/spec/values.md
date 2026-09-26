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

- A type declares a destructor as `fn drop(self: Self)` in an `impl`
  block, returning nothing and taking no other parameter. Any other
  signature under that name is a compile-time error. The name is
  reserved, so a free function called `drop` cannot be used to end a
  value early; call the method instead.
- The destructor consumes the value. Ending a value by value is a move,
  so a value that has already left is never ended twice, whether it was
  passed to a function, returned, or ended explicitly.
- A type that is `Copy` cannot have a destructor. `Copy` is structural
  (see above), so such a type has nothing a destructor could release,
  and every copy of it would be ended.
- Scope exit runs the destructors of the values that block declared,
  innermost first, and a `return` runs the destructors of everything
  still alive. A destructor that ends a value of a generic type is
  resolved per instantiation.
- A type with no destructor of its own still ends what it holds: the
  compiler ends each destructible struct field. Reaching a destructor
  through an enum variant or an array element needs the containing type
  to declare its own `drop`, and the compiler warns when it cannot place
  one.
- Discarding a value that holds something with a destructor (`_ := v`)
  is a compile-time error: nothing would be left to own it.
- Destructors do not run on the panic path, which aborts (see
  `errors.md`). An `alloc` block whose owning value is lost to a panic
  or a crash is leaked.

## Shadowing

- Shadowing is permitted. Each shadowing declaration introduces a
  fresh binding; the compiler desugars shadowing before name
  resolution, so later analyses only ever see distinct bindings.
- A `:=` declaration MUST bind only names that are new in its scope;
  redeclaring is a compile-time error suggesting `=`.
