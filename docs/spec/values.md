# Values, Affinity, and Destruction (MVP)

## Value model

- Every value has a single owner. Assignment, argument passing, and
  `return` move ownership. Accessing a moved-from variable is a
  compile-time error (use-after-move MUST be rejected).
- Values are affine (use at most once): a value without a destructor
  ends at scope exit with no user code running. There is no
  must-consume checking.
- `Copy` is structural and opt-out-free: a type is `Copy` if and only
  if all of its fields are `Copy`. Primitive machine types and
  shared references (`&T`) are `Copy`; exclusive references (`&mut T`)
  are move-only. No syntax exists to declare or suppress `Copy`. A
  declared destructor is the one exception: it makes the type
  move-only, whatever its fields are.

## Destruction

- A type declares a destructor as `fn drop(self: Self)` in an `impl`
  block, returning nothing and taking no other parameter. Any other
  signature under that name is a compile-time error. The name is
  reserved, so a free function called `drop` cannot be used to end a
  value early; call the method instead.
- The destructor consumes the value. Ending a value by value is a move,
  so a value that has already left is never ended twice, whether it was
  passed to a function, returned, or ended explicitly.
- Writing over a place that still owns a value ends that value before
  the new one lands. Writing into a place whose value has already left
  is re-initialization, and ends nothing.
- A view (`str`, a slice, a reference) cannot have a destructor; it
  has nothing to end. Any other type may declare one, and the
  declaration makes it move-only.
- Scope exit runs the destructors of the values that block declared,
  innermost first, and a `return` runs the destructors of everything
  still alive. A destructor that ends a value of a generic type is
  resolved per instantiation.
- A type with no destructor of its own still ends what it holds: the
  compiler ends each destructible struct or tuple field, the live
  variant's destructible fields after a discriminant read, and each
  destructible array element.
- Ends have a stated order (ADR-0086): a block's values end in reverse
  declaration order, so the value declared last ends first and a
  borrower's end precedes the end of what it borrows; a `return` takes
  the result first, then ends what is still alive in the same order.
- A struct's or tuple's fields end in declaration order, an enum's
  live payload in declaration order, and an array's elements in index
  order. A type with its own destructor runs it first; the fields of
  its receiver end when the destructor's scope exits, in the same
  order. The std's sequences end their elements from first to last.
- There is no `forget` and no `ManuallyDrop`-shaped wrapper: a value's
  destructor runs at its end, or the program panics and leaves what it
  owned to the process. Deliberately leaking a value has no spelling.
- Discarding a value ends it where it stands: `_ := v` runs the
  destructor, and a bare expression statement whose value is not `()`
  warns and does the same. A wildcard binding in a pattern ends what it
  discards the same way.
- Destructors do not run on the panic path, which aborts (see
  `errors.md`). An `alloc` block whose owning value is lost to a panic
  or a crash is leaked.

## Shadowing

- Shadowing is permitted. Each shadowing declaration introduces a
  fresh binding; the compiler desugars shadowing before name
  resolution, so later analyses only ever see distinct bindings.
- A `:=` declaration MUST bind only names that are new in its scope;
  redeclaring is a compile-time error suggesting `=`.
